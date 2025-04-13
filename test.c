#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>

#define MYDEV_PATH "/dev/a6"
#define CDRV_IOC_MAGIC 'Z'
#define E2_IOCMODE1 _IO(CDRV_IOC_MAGIC, 1)
#define E2_IOCMODE2 _IO(CDRV_IOC_MAGIC, 2)

#define BUFFER_SIZE 1024

int global_fd = -1;
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
int mode_change_in_progress = 0;

// print pass/fail
void test_result(int result) {
    if (result) {
        printf("Passed!\n");
    } else {
        printf("Failed!\n");
    }
}

// alarm for finding deadlock, 5 seconds
void setup_timeout(const char *test_name) {
    printf("Running test: %s\n", test_name);
    alarm(5);
}

// in the case of a timeout, prob deadlock
void timeout_handler(int signum) {
    printf("\nTest timed out, possible deadlock\n");
    if (global_fd >= 0) {
        close(global_fd);
    }
    exit(EXIT_FAILURE);
}

int open_device() {
    int fd = open(MYDEV_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
    }
    return fd;
}

// Test 1: opening at the same time in MODE1
void test_simultaneous_open() {
    setup_timeout("Open simultaneously in MODE1");
    
    int fd1 = open_device();
    if (fd1 < 0) return;
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        close(fd1);
        return;
    }
    
    // child
    if (pid == 0) {
        int fd2 = open_device();
        if (fd2 < 0) {
            exit(EXIT_FAILURE);
        }
       
        sleep(2);
        close(fd2);
        exit(EXIT_SUCCESS);

    // parent
    } else {  
        int status;
        sleep(1);
        close(fd1);
        waitpid(pid, &status, 0);
        
        // test passes if child exited normally
        test_result(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
}


// Test 2: change mode while multiple processes have device open
void test_mode_change_multiple_opens() {
    setup_timeout("Change mode with multiple open");
    
    int fd1 = open_device();
    if (fd1 < 0) return;
    
    // change to mode2 so you can have multiple opens
    int ret = ioctl(fd1, E2_IOCMODE2, 0);
    if (ret < 0) {
        perror("ioctl MODE2 failed");
        close(fd1);
        return;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        close(fd1);
        return;
    }
    
    if (pid == 0) {  // child process
        int fd2 = open_device();
        if (fd2 < 0) {
            exit(EXIT_FAILURE);
        }
        sleep(2);
        
        // try to read/write to make sure it still works
        char buffer[10] = {0};
        ssize_t bytes = read(fd2, buffer, 5);
        
        close(fd2);
        exit(bytes >= 0 ? EXIT_SUCCESS : EXIT_FAILURE);

    } else {  // parent process
        sleep(1);
        
        // switch back to mode1, should wait on child process
        ret = ioctl(fd1, E2_IOCMODE1, 0);
        if (ret < 0) {
            perror("ioctl MODE1 failed");
        }
        
        int status;
        waitpid(pid, &status, 0);
        close(fd1);
        
        // test passes if mode change completed successfully
        test_result(ret == 0 && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
}


// Test 3: multiple read/write with multiple threads in MODE2
// directly below is the function used by the threads, underneath is the creator of the threads
void *read_write_thread(void *arg) {
    int fd = open_device();
    if (fd < 0) {
        return (void *)-1;
    }
    
    char write_buf[BUFFER_SIZE];
    char read_buf[BUFFER_SIZE];
    
    // Fill write buffer with random data
    for (int i = 0; i < BUFFER_SIZE; i++) {
        write_buf[i] = (char)(rand() % 256);
    }
    
    // Perform multiple read/write operations
    for (int i = 0; i < 10; i++) {
        if (write(fd, write_buf, BUFFER_SIZE) != BUFFER_SIZE) {
            perror("write failed");
            close(fd);
            return (void *)-1;
        }
        
        lseek(fd, 0, SEEK_SET);
        
        if (read(fd, read_buf, BUFFER_SIZE) != BUFFER_SIZE) {
            perror("read failed");
            close(fd);
            return (void *)-1;
        }
    }
    
    close(fd);
    return (void *)0;
}

void test_multi_IO() {
    setup_timeout("Multi read/write with multi threads (MODE2)");
    
    int fd = open_device();
    if (fd < 0) return;
    
    // switch to MODE2 (if not already)
    int ret = ioctl(fd, E2_IOCMODE2, 0);
    if (ret < 0) {
        perror("ioctl MODE2 failed");
        close(fd);
        return;
    }
    
    pthread_t threads[5];
    int success = 1;
    
    // make all da threads
    for (int i = 0; i < 5; i++) {
        if (pthread_create(&threads[i], NULL, read_write_thread, NULL) != 0) {
            perror("pthread_create failed");
            success = 0;
            break;
        }
    }
    
    // join all da threads
    for (int i = 0; i < 5; i++) {
        void *thread_result;
        if (pthread_join(threads[i], &thread_result) != 0) {
            perror("pthread_join failed");
            success = 0;
        } else if (thread_result != 0) {
            success = 0;
        }
    }
    
    ret = ioctl(fd, E2_IOCMODE1, 0);
    close(fd);   
    test_result(success);
}

// Test 4: change mode during read/write
// similar to last test, below is the thread function and below that is another thread function and below that is the creator of the threads
void *io_thread(void *arg) {
    int fd = *(int *)arg;
    char buffer[BUFFER_SIZE] = {0};
    
    // many writes!!1!
    for (int i = 0; i < 100; i++) {
        lseek(fd, 0, SEEK_SET);
        ssize_t result = write(fd, buffer, BUFFER_SIZE);
        if (result != BUFFER_SIZE) {
            return (void *)-1;
        }
    }
    
    return (void *)0;
}

void *mode_change_thread(void *arg) {
    int fd = *(int *)arg;
    sleep(1); 
    
    // mode change moment
    pthread_mutex_lock(&global_mutex);
    mode_change_in_progress = 1;
    pthread_mutex_unlock(&global_mutex);
    
    int ret = ioctl(fd, E2_IOCMODE2, 0);
    if (ret < 0) {
        perror("ioctl MODE2 failed");
        return (void *)-1;
    }
    
    ret = ioctl(fd, E2_IOCMODE1, 0);
    if (ret < 0) {
        perror("ioctl MODE1 failed");
        return (void *)-1;
    }
    
    pthread_mutex_lock(&global_mutex);
    mode_change_in_progress = 0;
    pthread_mutex_unlock(&global_mutex);
    
    return (void *)0;
}

void test_mode_change_during_IO() {
    setup_timeout("Mode Change During I/O");

    int fd = open_device();
    if (fd < 0) return;

    global_fd = fd;

    pthread_t io_thread_id, mode_thread_id;

    // writing thread
    if (pthread_create(&io_thread_id, NULL, io_thread, &fd) != 0) {
        perror("pthread_create failed");
        close(fd);
        return;
    }

    // mode change thread
    if (pthread_create(&mode_thread_id, NULL, mode_change_thread, &fd) != 0) {
        perror("pthread_create failed");
        pthread_cancel(io_thread_id);
        close(fd);
        return;
    }

    void* io_result, * mode_result;
    int success = 1;

    if (pthread_join(io_thread_id, &io_result) != 0 || io_result != 0) {
        success = 0;
    }

    if (pthread_join(mode_thread_id, &mode_result) != 0 || mode_result != 0) {
        success = 0;
    }

    close(fd);
    global_fd = -1;

    test_result(success);
}


int main() {
    printf("Deadlock test cases:\n");
    
    // timeout detection stuff
    signal(SIGALRM, timeout_handler);
    srand(time(NULL));
    
    // tests
    test_simultaneous_open();
    test_mode_change_multiple_opens();
    test_multi_IO();
    test_mode_change_during_IO();
    
    return 0;
}