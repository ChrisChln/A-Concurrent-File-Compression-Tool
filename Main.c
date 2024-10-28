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
#include <sys/select.h>

#define NUM_WORKERS 4
#define MAX_FILENAME_LENGTH 256
#define MAX_PATH_LENGTH 4096
#define BUFFER_SIZE 4096
#define MAX_COMMAND_LENGTH (MAX_PATH_LENGTH * 2 + 100)
#define SHUTDOWN_SIGNAL "SHUTDOWN"
#define OUTPUT_DIR "compressed_files"
#define TIMEOUT 30 // Timeout in seconds

typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    pid_t worker_pid;
    time_t start_time;
    time_t end_time;
    char status[10];
} CompressionRecord;

enum WorkerStatus {
    WORKER_IDLE,
    WORKER_BUSY,
    WORKER_ERROR,
    WORKER_TERMINATED
};

typedef struct {
    int input_pipe[2];
    int output_pipe[2];
    enum WorkerStatus status;
    pid_t pid;
} WorkerPipes;

void close_unused_pipe_ends(WorkerPipes *pipes, int is_worker) {
    if (is_worker) {
        close(pipes->input_pipe[1]);
        close(pipes->output_pipe[0]);
    } else {
        close(pipes->input_pipe[0]);
        close(pipes->output_pipe[1]);
    }
}

void write_log_entry(const CompressionRecord *record) {
    FILE *log_file = fopen("compression.log", "a");
    if (log_file) {
        char start_time_str[26];
        char end_time_str[26];
        ctime_r(&record->start_time, start_time_str);
        ctime_r(&record->end_time, end_time_str);
        start_time_str[strlen(start_time_str) - 1] = '\0'; // Remove newline character
        end_time_str[strlen(end_time_str) - 1] = '\0'; // Remove newline character

        fprintf(log_file, "File: %s, Worker PID: %d, Start Time: %s, End Time: %s, Status: %s\n",
                record->filename, record->worker_pid, start_time_str, end_time_str, record->status);
        fclose(log_file);
    }
}

void handle_worker_error(const char *filename, pid_t worker_pid, const char *error_message) {
    CompressionRecord record;
    strncpy(record.filename, filename, MAX_FILENAME_LENGTH);
    record.worker_pid = worker_pid;
    record.start_time = time(NULL);
    record.end_time = time(NULL);
    strncpy(record.status, "Error", sizeof(record.status));
    write_log_entry(&record);
    fprintf(stderr, "Error processing file %s by worker %d: %s\n", filename, worker_pid, error_message);
}

void worker_process(WorkerPipes *pipes) {
    close_unused_pipe_ends(pipes, 1);
    char filename[MAX_FILENAME_LENGTH];
    
    while (read(pipes->input_pipe[0], filename, sizeof(filename)) > 0) {
        if (strcmp(filename, SHUTDOWN_SIGNAL) == 0) {
            break;
        }
        CompressionRecord record;
        strncpy(record.filename, filename, MAX_FILENAME_LENGTH);
        record.worker_pid = getpid();
        record.start_time = time(NULL);
        
        // Perform compression
        char command[MAX_COMMAND_LENGTH];
        snprintf(command, sizeof(command), "gzip -c %s > %s/%s.gz", filename, OUTPUT_DIR, filename);
        int result = system(command);
        
        record.end_time = time(NULL);
        if (result == 0) {
            strncpy(record.status, "Success", sizeof(record.status));
            write(pipes->output_pipe[1], "Success", 7);
        } else {
            strncpy(record.status, "Error", sizeof(record.status));
            write(pipes->output_pipe[1], "Error", 5);
        }
        write_log_entry(&record);
    }
    
    close(pipes->input_pipe[0]);
    close(pipes->output_pipe[1]);
    exit(0);
}

void cleanup_resources(WorkerPipes *pipes, pid_t *pids) {
    for (int i = 0; i < NUM_WORKERS; i++) {
        close(pipes[i].input_pipe[1]);
        close(pipes[i].output_pipe[0]);
        waitpid(pids[i], NULL, 0);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <source_archive.tar.gz>\n", argv[0]);
        return 1;
    }
    
    // Decompress the provided .tar.gz file
    char decompress_command[MAX_COMMAND_LENGTH];
    snprintf(decompress_command, sizeof(decompress_command), "tar -xzf %s", argv[1]);
    if (system(decompress_command) != 0) {
        fprintf(stderr, "Failed to decompress the archive.\n");
        return 1;
    }
    
    // Create output directory
    mkdir(OUTPUT_DIR, 0777);
    
    WorkerPipes pipes[NUM_WORKERS];
    pid_t pids[NUM_WORKERS];
    fd_set read_fds;
    int max_fd = 0;
    
    // Create pipes and fork workers
    for (int i = 0; i < NUM_WORKERS; i++) {
        pipe(pipes[i].input_pipe);
        pipe(pipes[i].output_pipe);
        if ((pids[i] = fork()) == 0) {
            worker_process(&pipes[i]);
        }
        close_unused_pipe_ends(&pipes[i], 0);
        pipes[i].status = WORKER_IDLE;
        pipes[i].pid = pids[i];
        if (pipes[i].output_pipe[0] > max_fd) {
            max_fd = pipes[i].output_pipe[0];
        }
    }
    
    // Open directory and distribute files
    DIR *dir = opendir(".");
    struct dirent *entry;
    struct stat file_stat;
    while ((entry = readdir(dir)) != NULL) {
        if (stat(entry->d_name, &file_stat) == 0 && S_ISREG(file_stat.st_mode)) {
            int assigned = 0;
            while (!assigned) {
                FD_ZERO(&read_fds);
                for (int i = 0; i < NUM_WORKERS; i++) {
                    if (pipes[i].status == WORKER_IDLE) {
                        write(pipes[i].input_pipe[1], entry->d_name, strlen(entry->d_name) + 1);
                        pipes[i].status = WORKER_BUSY;
                        assigned = 1;
                        break;
                    }
                    FD_SET(pipes[i].output_pipe[0], &read_fds);
                }
                if (!assigned) {
                    struct timeval timeout;
                    timeout.tv_sec = TIMEOUT;
                    timeout.tv_usec = 0;
                    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
                    if (ret == -1) {
                        perror("select");
                        exit(EXIT_FAILURE);
                    } else if (ret == 0) {
                        fprintf(stderr, "Timeout occurred! No data after %d seconds.\n", TIMEOUT);
                        exit(EXIT_FAILURE);
                    }
                    for (int i = 0; i < NUM_WORKERS; i++) {
                        if (FD_ISSET(pipes[i].output_pipe[0], &read_fds)) {
                            char status[10];
                            read(pipes[i].output_pipe[0], status, sizeof(status));
                            pipes[i].status = WORKER_IDLE;
                            CompressionRecord record;
                            strncpy(record.filename, entry->d_name, MAX_FILENAME_LENGTH);
                            record.worker_pid = pipes[i].pid;
                            record.start_time = time(NULL);
                            record.end_time = time(NULL);
                            strncpy(record.status, status, sizeof(record.status));
                            write_log_entry(&record);
                        }
                    }
                }
            }
        }
    }
    closedir(dir);
    
    // Send shutdown signal to workers
    for (int i = 0; i < NUM_WORKERS; i++) {
        write(pipes[i].input_pipe[1], SHUTDOWN_SIGNAL, strlen(SHUTDOWN_SIGNAL) + 1);
    }
    
    // Cleanup resources
    cleanup_resources(pipes, pids);

    return 0;
}
