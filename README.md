#Project-3-F24
**A Concurrent File Compression Tool**

**Overview**
This project showcases how multiple processes work together to achieve parallel file compression while communicating through pipes. The tool efficiently compresses multiple files at once by distributing the workload across several worker processes. Each worker process reports its progress back to the main process, which manages synchronization, logging, and overall task coordination.

**Project Description**
The tool begins with a Main Process that spawns four Worker Processes. Each worker is assigned a file to compress and uses Inter-Process Communication (IPC) via pipes to receive tasks and report back progress. The main process monitors the workers, logs task completion, and handles error reporting. All file compressions are performed using gzip, with the output files stored in a separate directory.

**Key Features**
1. Main Process
Spawns four worker processes using fork().
Distributes files to worker processes by sending file paths through a pipe.
Monitors and logs the progress of each worker, ensuring that all files are compressed efficiently.
Manages worker shutdown after all tasks are completed.
2. Worker Processes
Each worker compresses one file at a time using the gzip utility.
Reports completion status and errors back to the main process via pipes.
Continuously waits for tasks until receiving a shutdown signal.
3. Inter-Process Communication (IPC)
Pipes are used for sending file names from the main process to workers.
Workers return status updates, including success or failure, through the pipes.
4. Error Handling
If a file is not found or a compression error occurs, workers report errors back to the main process.
The main process logs all errors to a log file.
5. Performance Monitoring
The tool demonstrates the performance benefits of parallel processing by compressing multiple files concurrently.
The main process records the start and end times of each worker task.

Installation and Compilation
**Prerequisites**
Linux or macOS
GCC compiler for compiling C programs
gzip and tar utilities for compression
Compilation

To compile the tool, use the following command:
-gcc -o file_compressor Main.c

You can prepare test files for compression with the following commands:
-mkdir testfiles_folder
-cd testfiles_folder
-echo "This is test file 1" > file1.txt
-echo "This is test file 2" > file2.txt
-echo "This is test file 3" > file3.txt
-tar -czf ../testfiles.tar.gz *.txt
-cd ..

Run the program with the compressed archive testfiles.tar.gz:
-./file_compressor testfiles.tar.gz

This will:

-Decompress the archive.
-Distribute the files to worker processes for compression.
-Store the compressed files in a compressed_files/ directory.
-Log the status of each task in compression.log.


**Requirements**
-The main process must handle decompression of the provided .tar.gz archive.
-Worker processes must compress individual files and report progress back to the main process.
-The main process must manage logging and ensure proper shutdown of workers after all tasks are complete.

**Technologies Used**
-Languages: C (using fork() and pipes)
-Compression Tools: gzip, tar
-IPC: Pipes for communication between the main and worker processes
