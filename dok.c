#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <regex.h>

#define MAX_LINE_LENGTH 2048
#define MAX_ITEMS 200
#define MAX_NAME_LENGTH 256
#define MAX_CONTENT_LENGTH 4096
#define MAX_PATH_LENGTH 512
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

// Parameter information - MOVED BEFORE function_t
typedef struct {
    char name[MAX_NAME_LENGTH];
    char type[MAX_NAME_LENGTH];
    char description[MAX_CONTENT_LENGTH];
    int is_pointer;
    int is_array;
    int is_const;
} parameter_t;

// Function information
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
    // Auto-parsed parameter information
    parameter_t parsed_params[20];  // Support up to 20 parameters
    int param_count;
    char return_type[MAX_NAME_LENGTH];
} function_t;

// File information
typedef struct {
    char filename[MAX_PATH_LENGTH];
    char full_path[MAX_PATH_LENGTH];
    function_t functions[MAX_ITEMS];
    int function_count;
} source_file_t;

// Global state
typedef struct {
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
} doc_system_t;

doc_system_t docs;

// Terminal handling
struct termios orig_termios;

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
void parse_function_parameters(const char *signature, function_t *func);
void generate_parameter_documentation(function_t *func, char *output);

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

// Parse a single parameter string like "const char *name" or "int count"
void parse_parameter(const char *param_str, parameter_t *param) {
    char temp[MAX_NAME_LENGTH];
    strcpy(temp, param_str);
    trim_whitespace(temp);
    
    // Initialize parameter
    strcpy(param->name, "");
    strcpy(param->type, "");
    strcpy(param->description, "");
    param->is_pointer = 0;
    param->is_array = 0;
    param->is_const = 0;
    
    if (strlen(temp) == 0) return;
    
    // Check for const keyword
    if (strncmp(temp, "const ", 6) == 0) {
        param->is_const = 1;
        memmove(temp, temp + 6, strlen(temp) - 5);
        trim_whitespace(temp);
    }
    
    // Find the parameter name (last identifier)
    char *tokens[10];
    int token_count = 0;
    
    // Make a copy for tokenization
    char *temp_copy = strdup(temp);
    char *token = strtok(temp_copy, " \t");
    while (token && token_count < 10) {
        tokens[token_count] = strdup(token);
        token_count++;
        token = strtok(NULL, " \t");
    }
    
    if (token_count == 0) {
        free(temp_copy);
        return;
    }
    
    // Last token is usually the name (unless it's all pointers/arrays)
    char *potential_name = tokens[token_count - 1];
    
    // Handle pointer/array syntax in the name
    char *name_start = potential_name;
    while (*name_start == '*') {
        param->is_pointer = 1;
        name_start++;
    }
    
    // Check for array syntax
    char *bracket = strchr(name_start, '[');
    if (bracket) {
        param->is_array = 1;
        *bracket = '\0';  // Remove array part from name
    }
    
    // Extract clean parameter name
    strcpy(param->name, name_start);
    
    // Build type string from remaining tokens
    char type_str[MAX_NAME_LENGTH] = "";
    for (int i = 0; i < token_count - 1; i++) {
        if (strlen(type_str) > 0) strcat(type_str, " ");
        strcat(type_str, tokens[i]);
    }
    
    // Add back pointer indicators that were in the type part
    for (int i = 0; i < token_count; i++) {
        char *ptr_check = tokens[i];
        while (*ptr_check == '*') {
            param->is_pointer = 1;
            ptr_check++;
        }
    }
    
    strcpy(param->type, type_str);
    
    // Generate auto-description based on parameter name and type
    if (strstr(param->name, "count") || strstr(param->name, "size") || strstr(param->name, "len")) {
        strcpy(param->description, "Size/count parameter");
    } else if (strstr(param->name, "buffer") || strstr(param->name, "buf")) {
        strcpy(param->description, "Buffer for data storage");
    } else if (strstr(param->name, "filename") || strstr(param->name, "file")) {
        strcpy(param->description, "File path or name");
    } else if (strstr(param->name, "callback") || strstr(param->name, "cb")) {
        strcpy(param->description, "Callback function");
    } else if (param->is_pointer && strstr(param->type, "char")) {
        strcpy(param->description, "String parameter");
    } else if (param->is_pointer) {
        strcpy(param->description, "Pointer parameter");
    } else {
        strcpy(param->description, "Parameter");
    }
    
    // Clean up allocated tokens
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    free(temp_copy);
}

// Extract return type from function signature
void extract_return_type(const char *signature, char *return_type) {
    char temp[MAX_NAME_LENGTH];
    strcpy(temp, signature);
    
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
        strcpy(return_type, temp);
    }
}

// Parse function parameters from signature
void parse_function_parameters(const char *signature, function_t *func) {
    func->param_count = 0;
    
    // Extract return type
    extract_return_type(signature, func->return_type);
    
    // Find parameter list
    char *paren_start = strchr(signature, '(');
    char *paren_end = strrchr(signature, ')');
    
    if (!paren_start || !paren_end || paren_end <= paren_start) {
        return;
    }
    
    // Extract parameter string
    int param_len = paren_end - paren_start - 1;
    if (param_len <= 0) return;
    
    char param_string[MAX_LINE_LENGTH];
    strncpy(param_string, paren_start + 1, param_len);
    param_string[param_len] = '\0';
    trim_whitespace(param_string);
    
    // Handle void parameter list
    if (strcmp(param_string, "void") == 0 || strlen(param_string) == 0) {
        return;
    }
    
    // Split by commas and parse each parameter
    char *param_copy = strdup(param_string);
    char *param_token = strtok(param_copy, ",");
    
    while (param_token && func->param_count < 20) {
        parse_parameter(param_token, &func->parsed_params[func->param_count]);
        if (strlen(func->parsed_params[func->param_count].name) > 0) {
            func->param_count++;
        }
        param_token = strtok(NULL, ",");
    }
    
    free(param_copy);
}

// Generate formatted parameter documentation
void generate_parameter_documentation(function_t *func, char *output) {
    if (func->param_count == 0) {
        strcpy(output, "No parameters");
        return;
    }
    
    strcpy(output, "");
    
    for (int i = 0; i < func->param_count; i++) {
        parameter_t *param = &func->parsed_params[i];
        
        char param_line[MAX_CONTENT_LENGTH];
        
        // Format: @param name (type) - description
        snprintf(param_line, sizeof(param_line), "@param %s (%s%s%s%s) - %s",
                param->name,
                param->is_const ? "const " : "",
                param->type,
                param->is_pointer ? "*" : "",
                param->is_array ? "[]" : "",
                param->description);
        
        if (strlen(output) > 0) {
            strcat(output, "\n");
        }
        strcat(output, param_line);
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
            
            // Store signature
            strncpy(func->signature, line, MAX_NAME_LENGTH - 1);
            func->signature[MAX_NAME_LENGTH - 1] = '\0';
            
            // Extract function name
            if (extract_function_name(line, func->name)) {
                strcpy(func->filename, file->filename);
                func->line_number = line_num;
                func->is_documented = 0;
                
                // Parse function parameters automatically
                parse_function_parameters(line, func);
                
                // Initialize documentation fields
                strcpy(func->description, "");
                strcpy(func->return_value, "");
                strcpy(func->example, "");
                strcpy(func->notes, "");
                
                // Auto-generate parameter documentation
                generate_parameter_documentation(func, func->parameters);
                
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
            strcpy(file->filename, entry->d_name);
            strcpy(file->full_path, entry->d_name);
            
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
            strcpy(current_func_name, line + 10);
        } else if (strncmp(line, "FILE: ", 6) == 0) {
            strcpy(current_filename, line + 6);
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
                    strcpy(func->description, line + 13);
                    func->is_documented = 1;
                } else if (strncmp(line, "PARAMETERS: ", 12) == 0) {
                    strcpy(func->parameters, line + 12);
                } else if (strncmp(line, "RETURN: ", 8) == 0) {
                    strcpy(func->return_value, line + 8);
                } else if (strncmp(line, "EXAMPLE: ", 9) == 0) {
                    strcpy(func->example, line + 9);
                } else if (strncmp(line, "NOTES: ", 7) == 0) {
                    strcpy(func->notes, line + 7);
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
    printf(BOLD CYAN "═══════════════════════════════════════════════════════════════════════════════\n");
    printf("                      DYNAMIC C PROJECT DOCUMENTATION                        \n");
    printf("═══════════════════════════════════════════════════════════════════════════════\n" RESET);
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
    printf("Use ↑/↓ to navigate, ENTER to view functions, 'p' to print file docs, 'r' to rescan, 's' to search, 'u' for undocumented, 'q' to quit\n\n");
    
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
    printf("Press 'e' to edit documentation, 'v' to view source, 'a' to view auto-parsed info, 'b' to go back\n\n");
    
    printf(BOLD CYAN "File: " RESET "%s:%d\n", func->filename, func->line_number);
    printf(BOLD CYAN "Signature: " RESET "%s\n", func->signature);
    
    // Show auto-parsed information
    if (func->param_count > 0) {
        printf(BOLD CYAN "Return Type: " RESET "%s\n", func->return_type);
        printf(BOLD CYAN "Parameters (%d): " RESET, func->param_count);
        for (int i = 0; i < func->param_count; i++) {
            parameter_t *param = &func->parsed_params[i];
            printf("%s%s%s%s %s",
                   param->is_const ? "const " : "",
                   param->type,
                   param->is_pointer ? "*" : "",
                   param->is_array ? "[]" : "",
                   param->name);
            if (i < func->param_count - 1) printf(", ");
        }
        printf("\n");
    } else {
        printf(BOLD CYAN "Parameters: " RESET "None\n");
    }
    
    printf("\n");
    
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
        printf(BLUE "Auto-generated parameter documentation is available as a starting point.\n" RESET);
    }
}

void display_auto_parsed_info() {
    clear_screen();
    display_header();
    
    function_t *func = &docs.files[docs.current_file].functions[docs.current_function];
    
    printf(BOLD GREEN "\nAUTO-PARSED INFORMATION: %s\n" RESET, func->name);
    printf("Press any key to go back\n\n");
    
    printf(BOLD CYAN "Return Type: " RESET "%s\n\n", func->return_type);
    
    if (func->param_count > 0) {
        printf(BOLD CYAN "Parsed Parameters:\n" RESET);
        for (int i = 0; i < func->param_count; i++) {
            parameter_t *param = &func->parsed_params[i];
            printf("  %d. " BOLD "%s" RESET "\n", i + 1, param->name);
            printf("     Type: %s%s%s%s\n",
                   param->is_const ? "const " : "",
                   param->type,
                   param->is_pointer ? "*" : "",
                   param->is_array ? "[]" : "");
            printf("     Auto-description: %s\n", param->description);
            printf("     Flags: %s%s%s\n",
                   param->is_const ? "const " : "",
                   param->is_pointer ? "pointer " : "",
                   param->is_array ? "array " : "");
            printf("\n");
        }
    } else {
        printf(BOLD CYAN "Parameters: " RESET "None (void function)\n");
    }
    
    printf(BOLD CYAN "Auto-generated parameter documentation:\n" RESET);
    printf("%s\n", func->parameters);
    
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
            trim_whitespace(trimmed);
            if (is_header && strlen(trimmed) > 0 && trimmed[strlen(trimmed)-1] == ';') {
                free(trimmed);
                break; // Just show the declaration line
            }
            free(trimmed);
            
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

// File documentation export
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
        
        printf("═══════════════════════════════════════════════════════════════════════════════\n");
        printf(BOLD CYAN "FUNCTION: %s" RESET " (Line %d)\n", func->name, func->line_number);
        printf("═══════════════════════════════════════════════════════════════════════════════\n");
        
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
    
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
    printf(BOLD GREEN "END OF DOCUMENTATION FOR %s\n" RESET, file->filename);
    printf("═══════════════════════════════════════════════════════════════════════════════\n");
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
        strcpy(func->description, temp_buffer);
    }
    
    // Parameters
    printf(BOLD "\nCurrent parameters:" RESET " %s\n", func->parameters);
    get_string_input("New parameters: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strcpy(func->parameters, temp_buffer);
    }
    
    // Return value
    printf(BOLD "\nCurrent return value:" RESET " %s\n", func->return_value);
    get_string_input("New return value: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strcpy(func->return_value, temp_buffer);
    }
    
    // Example
    printf(BOLD "\nCurrent example:" RESET " %s\n", func->example);
    get_string_input("New example: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strcpy(func->example, temp_buffer);
    }
    
    // Notes
    printf(BOLD "\nCurrent notes:" RESET " %s\n", func->notes);
    get_string_input("New notes: ", temp_buffer, MAX_CONTENT_LENGTH);
    if (strlen(temp_buffer) > 0) {
        strcpy(func->notes, temp_buffer);
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
                case 'a':
                    display_auto_parsed_info();
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
