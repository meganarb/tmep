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

/* Global variables */
int global_fd = -1;
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
int mode_change_in_progress = 0;
int tests_passed = 0;
int tests_failed = 0;

/* Utility function to report test results */
void report_test(const char *test_name, int result) {
    if (result) {
        printf("[PASS] %s\n", test_name);
        tests_passed++;
    } else {
        printf("[FAIL] %s\n", test_name);
        tests_failed++;
    }
}

/* Set up an alarm to detect deadlocks */
void setup_timeout(const char *test_name) {
    printf("Running test: %s\n", test_name);
    alarm(5);
}

/* Signal handler for timeouts */
void timeout_handler(int signum) {
    printf("\n[TIMEOUT] Test has timed out - possible deadlock detected\n");
    if (global_fd >= 0) {
        close(global_fd);
    }
    exit(EXIT_FAILURE);
}

/* Helper function to open the device */
int open_device() {
    int fd = open(MYDEV_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
    }
    return fd;
}


/* Test 2: Concurrent open in MODE1 */
void test_concurrent_open_mode1() {
    setup_timeout("Concurrent Open in MODE1");
    
    int fd1 = open_device();
    if (fd1 < 0) return;
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        close(fd1);
        return;
    }
    
    if (pid == 0) {  // Child process
        int fd2 = open_device();
        if (fd2 < 0) {
            exit(EXIT_FAILURE);
        }
        
        // In MODE1, second open should block indefinitely
        // We'll sleep briefly then exit to avoid creating zombie
        sleep(2);
        close(fd2);
        exit(EXIT_SUCCESS);
    } else {  // Parent process
        int status;
        sleep(1);  // Give child time to attempt open
        
        // Now close our FD which should release the lock
        close(fd1);
        
        // Wait for child to complete
        waitpid(pid, &status, 0);
        
        // Test passes if child exited normally
        report_test("Concurrent Open in MODE1", WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
}


/* Test 4: Mode change while multiple processes have the device open */
void test_mode_change_multiple_opens() {
    setup_timeout("Mode Change with Multiple Opens");
    
    int fd1 = open_device();
    if (fd1 < 0) return;
    
    // Switch to MODE2 to allow multiple opens
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
    
    if (pid == 0) {  // Child process
        int fd2 = open_device();
        if (fd2 < 0) {
            exit(EXIT_FAILURE);
        }
        
        // Sleep to ensure parent has time to attempt mode change
        sleep(2);
        
        // Try to read/write to verify device is still functional
        char buffer[10] = {0};
        ssize_t bytes = read(fd2, buffer, 5);
        
        close(fd2);
        exit(bytes >= 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    } else {  // Parent process
        sleep(1);  // Give child time to open device
        
        // Now try to switch back to MODE1, should wait for child to close
        ret = ioctl(fd1, E2_IOCMODE1, 0);
        if (ret < 0) {
            perror("ioctl MODE1 failed");
        }
        
        // Wait for child to complete
        int status;
        waitpid(pid, &status, 0);
        
        close(fd1);
        
        // Test passes if mode change completed successfully
        report_test("Mode Change with Multiple Opens", ret == 0 && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
    }
}


/* Test 6: Multiple threads in MODE2 with read/write operations */
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

void test_concurrent_io_mode2() {
    setup_timeout("Concurrent I/O in MODE2");
    
    int fd = open_device();
    if (fd < 0) return;
    
    // Switch to MODE2
    int ret = ioctl(fd, E2_IOCMODE2, 0);
    if (ret < 0) {
        perror("ioctl MODE2 failed");
        close(fd);
        return;
    }
    
    pthread_t threads[5];
    int success = 1;
    
    // Create multiple threads performing I/O
    for (int i = 0; i < 5; i++) {
        if (pthread_create(&threads[i], NULL, read_write_thread, NULL) != 0) {
            perror("pthread_create failed");
            success = 0;
            break;
        }
    }
    
    // Join all threads
    for (int i = 0; i < 5; i++) {
        void *thread_result;
        if (pthread_join(threads[i], &thread_result) != 0) {
            perror("pthread_join failed");
            success = 0;
        } else if (thread_result != 0) {
            success = 0;
        }
    }
    
    // Switch back to MODE1
    ret = ioctl(fd, E2_IOCMODE1, 0);
    close(fd);
    
    report_test("Concurrent I/O in MODE2", success);
}

/* Test 7: Mode change during a read/write operation */
void *io_thread(void *arg) {
    int fd = *(int *)arg;
    char buffer[BUFFER_SIZE] = {0};
    
    // Perform a long running write operation
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
    
    // Give I/O thread time to start
    sleep(1);
    
    // Try mode change
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

void test_mode_change_during_io() {
    setup_timeout("Mode Change During I/O");
    
    int fd = open_device();
    if (fd < 0) return;
    
    global_fd = fd;
    
    pthread_t io_thread_id, mode_thread_id;
    
    // Create thread for I/O operations
    if (pthread_create(&io_thread_id, NULL, io_thread, &fd) != 0) {
        perror("pthread_create failed");
        close(fd);
        return;
    }
    
    // Create thread for mode changes
    if (pthread_create(&mode_thread_id, NULL, mode_change_thread, &fd) != 0) {
        perror("pthread_create failed");
        pthread_cancel(io_thread_id);
        close(fd);
        return;
    }
    
    // Join threads
    void *io_result, *mode_result;
    int success = 1;
    
    if (pthread_join(io_thread_id, &io_result) != 0 || io_result != 0) {
        success = 0;
    }
    
    if (pthread_join(mode_thread_id, &mode_result) != 0 || mode_result != 0) {
        success = 0;
    }
    
    close(fd);
    global_fd = -1;
    
    report_test("Mode Change During I/O", success);


int main() {
    printf("===== Device Driver Deadlock Test Program =====\n");
    
    // Set up signal handler for timeout detection
    signal(SIGALRM, timeout_handler);
    srand(time(NULL));
    
    // Run tests
    test_concurrent_open_mode1();
    test_concurrent_open_mode2();
    test_mode_change_multiple_opens();
    test_rapid_mode_changes();
    test_concurrent_io_mode2();
    test_mode_change_during_io();
    test_racing_mode_changes();
    
    // Basic mode switching test
    setup_timeout("Basic Mode Switching");
    int fd = open_device();
    if (fd >= 0) {
        pthread_t thread;
        int ret = pthread_create(&thread, NULL, test_mode_switching, &fd);
        if (ret == 0) {
            void *thread_ret;
            pthread_join(thread, &thread_ret);
            report_test("Basic Mode Switching", thread_ret == 0);
        } else {
            report_test("Basic Mode Switching", 0);
        }
        close(fd);
    }
    
    // Print summary
    printf("\n===== Test Summary =====\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}