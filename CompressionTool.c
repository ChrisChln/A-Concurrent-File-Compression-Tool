#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>

#define NUM_WORKERS 4
#define MAX_FILENAME_LENGTH 256
#define MAX_PATH_LENGTH 4096
#define BUFFER_SIZE 4096
#define MAX_COMMAND_LENGTH (MAX_PATH_LENGTH * 2 + 100)

// Enum definitions
enum WorkerStatus {
    WORKER_IDLE,
    WORKER_BUSY,
    WORKER_DONE,
    WORKER_ERROR,
    WORKER_TERMINATED
};

// Define structs before use
struct PipeSet {
    int input_pipe[2];
    int output_pipe[2];
};

struct WorkerState {
    enum WorkerStatus status;
    pid_t pid;
    int files_processed;
    time_t last_active;
};

struct CompressionRecord {
    char filename[MAX_FILENAME_LENGTH];
    int worker_id;
    time_t start_time;
    time_t end_time;
    int status;
    size_t original_size;
    size_t compressed_size;
    char error_message[256];
};

// Function declarations
void write_log_entry(const char *message);
int init_log_file(void);
int setup_directories(void);
int create_pipe_pair(struct PipeSet *pipes);
void close_unused_pipe_ends(struct PipeSet *pipes, int is_worker);
int find_idle_worker(struct WorkerState *workers);
void cleanup_resources(struct PipeSet *pipes, struct WorkerState *workers);
int decompress_source_file(const char *source_file);
void worker_process(int worker_id, struct PipeSet *pipe);

// Global variables
FILE *log_file = NULL;
char source_dir[MAX_PATH_LENGTH];
char output_dir[MAX_PATH_LENGTH];

// Function implementations
void write_log_entry(const char *message) {
    if (log_file != NULL) {
        time_t now = time(NULL);
        char timestamp[64];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(log_file, "[%s] %s\n", timestamp, message);
        fflush(log_file);
    }
}

int init_log_file(void) {
    char log_path[MAX_PATH_LENGTH];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    snprintf(log_path, sizeof(log_path), "compression_%04d%02d%02d_%02d%02d%02d.log",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    log_file = fopen(log_path, "w");
    if (log_file == NULL) {
        perror("Failed to create log file");
        return -1;
    }
    
    fprintf(log_file, "Compression Task Log - Started at: %s\n", ctime(&now));
    fflush(log_file);
    return 0;
}

int setup_directories(void) {
    snprintf(source_dir, sizeof(source_dir), "./source_files");
    snprintf(output_dir, sizeof(output_dir), "./compressed_files");
    
    // Create directories with proper error checking
    if (mkdir(source_dir, 0755) == -1 && errno != EEXIST) {
        perror("Failed to create source directory");
        return -1;
    }
    
    if (mkdir(output_dir, 0755) == -1 && errno != EEXIST) {
        perror("Failed to create output directory");
        return -1;
    }
    
    return 0;
}

int create_pipe_pair(struct PipeSet *pipes) {
    if (pipe(pipes->input_pipe) == -1) {
        perror("Failed to create input pipe");
        return -1;
    }
    
    if (pipe(pipes->output_pipe) == -1) {
        close(pipes->input_pipe[0]);
        close(pipes->input_pipe[1]);
        perror("Failed to create output pipe");
        return -1;
    }
    
    // Set non-blocking mode for pipes
    fcntl(pipes->input_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(pipes->output_pipe[0], F_SETFL, O_NONBLOCK);
    
    return 0;
}

void close_unused_pipe_ends(struct PipeSet *pipes, int is_worker) {
    if (is_worker) {
        close(pipes->input_pipe[1]);
        close(pipes->output_pipe[0]);
    } else {
        close(pipes->input_pipe[0]);
        close(pipes->output_pipe[1]);
    }
}

int find_idle_worker(struct WorkerState *workers) {
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (workers[i].status == WORKER_IDLE) {
            return i;
        }
    }
    return -1;
}

void cleanup_resources(struct PipeSet *pipes, struct WorkerState *workers) {
    // Close all pipes
    for (int i = 0; i < NUM_WORKERS; i++) {
        close(pipes[i].input_pipe[0]);
        close(pipes[i].input_pipe[1]);
        close(pipes[i].output_pipe[0]);
        close(pipes[i].output_pipe[1]);
    }
    
    // Wait for all worker processes
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (workers[i].pid > 0) {
            waitpid(workers[i].pid, NULL, 0);
        }
    }
    
    if (log_file != NULL) {
        fclose(log_file);
    }
}

int decompress_source_file(const char *source_file) {
    char command[MAX_COMMAND_LENGTH];
    snprintf(command, sizeof(command), "tar -xzvf %s -C %s", source_file, source_dir);
    
    // Check if the file exists
    if (access(source_file, F_OK) == -1) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Source file does not exist: %s", source_file);
        write_log_entry(error_msg);
        perror("File access error");
        return -1;
    }
    
    // Check if we have read permission
    if (access(source_file, R_OK) == -1) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "No read permission for source file: %s", source_file);
        write_log_entry(error_msg);
        perror("File permission error");
        return -1;
    }
    
    int result = system(command);
    
    if (result != 0) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg), "Decompression failed. Command: %s", command);
        write_log_entry(error_msg);
        perror("System command failed");
        return -1;
    }
    
    write_log_entry("Decompression completed successfully.");
    return 0;
}

void worker_process(int worker_id, struct PipeSet *pipe) {
    close_unused_pipe_ends(pipe, 1); // Close unused ends for the worker
    char filename[MAX_FILENAME_LENGTH];
    
    while (read(pipe->input_pipe[0], filename, sizeof(filename)) > 0) {
        write_log_entry("Worker received file for processing.");
        // Perform compression (placeholder)
        sleep(1);  // Simulate work being done
        
        // Write back to the pipe (placeholder for response)
        write(pipe->output_pipe[1], "Processed", 9);
    }
    
    close(pipe->input_pipe[0]);
    close(pipe->output_pipe[1]);
    exit(0);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <source_archive.tar.gz>\n", argv[0]);
        return 1;
    }
    
    if (setup_directories() != 0 || init_log_file() != 0) {
        return 1;
    }
    
    if (decompress_source_file(argv[1]) != 0) {
        return 1;
    }

    struct PipeSet pipes[NUM_WORKERS];
    struct WorkerState workers[NUM_WORKERS];
    
    // Initialize workers and create pipes
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (create_pipe_pair(&pipes[i]) != 0) {
            write_log_entry("Failed to create pipes");
            return 1;
        }
        
        workers[i].status = WORKER_IDLE;
        workers[i].files_processed = 0;
    }
    
    // Create worker processes
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        
        if (pid < 0) {
            write_log_entry("Failed to create worker process");
            cleanup_resources(pipes, workers);
            return 1;
        }
        
        if (pid == 0) {
            // Child process
            for (int j = 0; j < NUM_WORKERS; j++) {
                if (j != i) {
                    close(pipes[j].input_pipe[0]);
                    close(pipes[j].input_pipe[1]);
                    close(pipes[j].output_pipe[0]);
                    close(pipes[j].output_pipe[1]);
                }
            }
            worker_process(i, &pipes[i]);
            exit(0);
        } else {
            workers[i].pid = pid;
            close_unused_pipe_ends(&pipes[i], 0);
        }
    }
    
    // Process files
    DIR *dir = opendir(source_dir);
    if (dir == NULL) {
        write_log_entry("Failed to open source directory");
        cleanup_resources(pipes, workers);
        return 1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        
        char filepath[MAX_PATH_LENGTH];
        int result = snprintf(filepath, sizeof(filepath), "%s/%s", 
                            source_dir, entry->d_name);
        
        if (result < 0 || result >= sizeof(filepath)) {
            write_log_entry("Error: Filepath too long");
            continue;
        }
        
        struct stat file_stat;
        if (stat(filepath, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
            int worker_id = find_idle_worker(workers);
            if (worker_id >= 0) {
                write(pipes[worker_id].input_pipe[1], entry->d_name, 
                      strlen(entry->d_name) + 1);
                workers[worker_id].status = WORKER_BUSY;
                
                char log_message[512];
                snprintf(log_message, sizeof(log_message), 
                        "Assigned file %s to worker %d", 
                        entry->d_name, worker_id);
                write_log_entry(log_message);
            }
        }
    }
    
    closedir(dir);
    cleanup_resources(pipes, workers);
    return 0;
}
