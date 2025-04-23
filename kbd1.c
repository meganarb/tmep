#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define LED_BUF_SIZE 1
#define SHM_NAME "/led_shm"
#define TERMINATE_SHM "/terminate_shm"

#define NO_EVENT        '#'
#define CAPSLOCK_PRESS  '@'
#define CAPSLOCK_RELEASE '&'
#define END_OF_INPUT    '$'  // Special marker for end of input

#define LED_ON  1
#define LED_OFF 0

// Forward declarations
struct usb_kbd;
struct input_dev;
struct urb;

typedef struct usb_kbd usb_kbd;
typedef struct input_dev input_dev;
typedef struct urb urb;

// Structure definitions
struct input_dev {
    void (*event)(struct input_dev* dev);
    int led;
};

struct urb {
    int endpoint_type;  // 0 for interrupt, 1 for control
    pthread_t thread;
    int active;
    void *context;
};

struct usb_kbd {
    struct input_dev* dev;

    int int_ep_fd;     // for reading from keyboard (interrupt endpoint)
    int ctrl_cmd_fd;   // for writing LED control commands
    int ctrl_ack_fd;   // for reading ACKs

    unsigned char* leds;
    unsigned char* terminate_flag;  // Points to shared memory for termination flag
    pthread_mutex_t leds_lock;
    
    // URBs for the endpoints
    struct urb* int_urb;
    struct urb* led_urb;
};

// Global variables
usb_kbd kbd;
int capslock_state = 0;
volatile int should_terminate = 0;  // Flag to indicate termination

// Function prototypes
void usb_submit_urb(urb* urb);
void* usb_kbd_irq(void* arg);
void* usb_kbd_led(void* arg);
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value);
void usb_kbd_event(struct input_dev* dev);
void print_char(char ch);
int usb_kbd_open(void);
void usb_kbd_close(void);
void cleanup_resources(void);

// Print character with capslock applied if needed
void print_char(char ch) {
    if (capslock_state && ch >= 'a' && ch <= 'z')
        ch = ch - 'a' + 'A';
    printf("%c", ch);
    fflush(stdout);
}

// Submit an URB (Universal Request Block) to start or continue endpoint handling
void usb_submit_urb(urb* urb) {
    if (!urb || urb->active || should_terminate) return;
    
    urb->active = 1;
    
    if (urb->endpoint_type == 0) { // Interrupt endpoint
        pthread_create(&urb->thread, NULL, usb_kbd_irq, urb);
    } else { // Control endpoint
        pthread_create(&urb->thread, NULL, usb_kbd_led, urb);
    }
    
    pthread_detach(urb->thread);
}

// Interrupt endpoint handler - processes key events
void* usb_kbd_irq(void* arg) {
    urb* irq_urb = (urb*)arg;
    usb_kbd* kbd_ptr = (usb_kbd*)irq_urb->context;
    irq_urb->active = 0;
    
    if (should_terminate || *(kbd_ptr->terminate_flag)) {
        should_terminate = 1;
        return NULL;
    }
    
    char ch;
    ssize_t n = read(kbd_ptr->int_ep_fd, &ch, 1);
    
    if (n <= 0 || ch == END_OF_INPUT) {
        should_terminate = 1;
        return NULL;
    }
    
    if (ch != NO_EVENT) {
        if (ch == CAPSLOCK_PRESS) {
            input_report_key(kbd_ptr, CAPSLOCK_PRESS, kbd_ptr->dev->led == LED_ON ? LED_OFF : LED_ON);
        } 
        else if (ch == CAPSLOCK_RELEASE) {
            // Just acknowledge capslock release
            input_report_key(kbd_ptr, CAPSLOCK_RELEASE, kbd_ptr->dev->led);
        }
        else {
            print_char(ch);
        }
    }
    
    // Re-submit URB to continue polling
    usb_submit_urb(irq_urb);
    
    return NULL;
}

// Control endpoint handler - handles LED state changes
void* usb_kbd_led(void* arg) {
    urb* led_urb = (urb*)arg;
    usb_kbd* kbd_ptr = (usb_kbd*)led_urb->context;
    led_urb->active = 0;
    
    if (should_terminate || *(kbd_ptr->terminate_flag)) {
        should_terminate = 1;
        return NULL;
    }
    
    // Send control command
    write(kbd_ptr->ctrl_cmd_fd, "C", 1);
    
    // Wait for ACK
    char ack;
    ssize_t n = read(kbd_ptr->ctrl_ack_fd, &ack, 1);
    
    if (n <= 0) {
        should_terminate = 1;
        return NULL;
    }
    
    // Re-submit the URB to be ready for next LED state change
    usb_submit_urb(led_urb);
    
    return NULL;
}

// Report a key event
void input_report_key(struct usb_kbd* kbd_ptr, unsigned int code, int value) {
    if (code == CAPSLOCK_PRESS || code == CAPSLOCK_RELEASE) {
        kbd_ptr->dev->led = value;
        usb_kbd_event(kbd_ptr->dev);
    }
}

// Process keyboard event and update LED state
void usb_kbd_event(struct input_dev* dev_ptr) {
    if (dev_ptr->led == LED_ON && !capslock_state) {
        capslock_state = 1;
    }
    else if (dev_ptr->led == LED_OFF && capslock_state) {
        capslock_state = 0;
    }

    // Write new LED state to shared memory with lock protection
    pthread_mutex_lock(&kbd.leds_lock);
    *(kbd.leds) = dev_ptr->led ? LED_ON : LED_OFF;
    pthread_mutex_unlock(&kbd.leds_lock);
    
    // Submit the LED URB to handle the LED state change
    usb_submit_urb(kbd.led_urb);
}

// Clean up resources
void cleanup_resources() {
    if (kbd.dev) free(kbd.dev);
    if (kbd.int_urb) free(kbd.int_urb);
    if (kbd.led_urb) free(kbd.led_urb);
    
    if (kbd.leds != MAP_FAILED && kbd.leds != NULL) {
        munmap(kbd.leds, LED_BUF_SIZE);
    }
    
    if (kbd.terminate_flag != MAP_FAILED && kbd.terminate_flag != NULL) {
        munmap(kbd.terminate_flag, 1);
    }
    
    if (kbd.int_ep_fd >= 0) close(kbd.int_ep_fd);
    if (kbd.ctrl_cmd_fd >= 0) close(kbd.ctrl_cmd_fd);
    if (kbd.ctrl_ack_fd >= 0) close(kbd.ctrl_ack_fd);
    
    pthread_mutex_destroy(&kbd.leds_lock);
}

// Close the USB device
void usb_kbd_close() {
    should_terminate = 1;
    cleanup_resources();
}

// Initialize and open the USB keyboard device
int usb_kbd_open(void) {
    // Initialize with invalid values
    kbd.int_ep_fd = -1;
    kbd.ctrl_cmd_fd = -1;
    kbd.ctrl_ack_fd = -1;
    kbd.leds = NULL;
    kbd.terminate_flag = NULL;
    kbd.dev = NULL;
    kbd.int_urb = NULL;
    kbd.led_urb = NULL;
    
    // Set up the input device
    input_dev* dev = malloc(sizeof(input_dev));
    if (!dev) return -1;
    
    dev->event = usb_kbd_event;
    dev->led = LED_OFF;
    kbd.dev = dev;
    
    // Initialize the mutex
    pthread_mutex_init(&kbd.leds_lock, NULL);
    
    // Open the pipes
    kbd.int_ep_fd = open("int_pipe", O_RDONLY | O_NONBLOCK);
    kbd.ctrl_cmd_fd = open("ctrl_cmd_pipe", O_WRONLY);
    kbd.ctrl_ack_fd = open("ctrl_ack_pipe", O_RDONLY);

    if (kbd.int_ep_fd < 0 || kbd.ctrl_cmd_fd < 0 || kbd.ctrl_ack_fd < 0) {
        perror("pipe open failed");
        cleanup_resources();
        return -1;
    }
    
    // Make int_ep_fd blocking again now that it's open
    int flags = fcntl(kbd.int_ep_fd, F_GETFL, 0);
    fcntl(kbd.int_ep_fd, F_SETFL, flags & ~O_NONBLOCK);

    // Set up shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        cleanup_resources();
        return -1;
    }

    kbd.leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (kbd.leds == MAP_FAILED) {
        perror("mmap failed");
        cleanup_resources();
        return -1;
    }
    
    // Set up shared memory for termination flag
    int terminate_fd = shm_open(TERMINATE_SHM, O_RDWR, 0666);
    if (terminate_fd == -1) {
        perror("shm_open for terminate flag failed");
        cleanup_resources();
        return -1;
    }
    
    kbd.terminate_flag = mmap(0, 1, PROT_READ | PROT_WRITE, MAP_SHARED, terminate_fd, 0);
    close(terminate_fd);
    
    if (kbd.terminate_flag == MAP_FAILED) {
        perror("mmap for terminate flag failed");
        cleanup_resources();
        return -1;
    }
    
    // Create URBs
    kbd.int_urb = malloc(sizeof(urb));
    kbd.led_urb = malloc(sizeof(urb));
    
    if (!kbd.int_urb || !kbd.led_urb) {
        perror("Failed to allocate URBs");
        cleanup_resources();
        return -1;
    }
    
    // Initialize URBs
    kbd.int_urb->endpoint_type = 0; // Interrupt endpoint
    kbd.int_urb->active = 0;
    kbd.int_urb->context = &kbd;
    
    kbd.led_urb->endpoint_type = 1; // Control endpoint
    kbd.led_urb->active = 0;
    kbd.led_urb->context = &kbd;
    
    // Submit the URBs to start the threads
    usb_submit_urb(kbd.int_urb);
    usb_submit_urb(kbd.led_urb);
    
    return 0;
}

// Signal handler for termination signals
void signal_handler(int sig) {
    should_terminate = 1;
}

// Driver function - now calls usb_kbd_open
int driver() {
    // Set up signal handling
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    if (usb_kbd_open() < 0) {
        fprintf(stderr, "Failed to open USB keyboard\n");
        return -1;
    }
    
    // Main loop - check for termination flag
    while (!should_terminate && !*(kbd.terminate_flag)) {
        // Check every 100ms
        usleep(100000);
    }
    
    usb_kbd_close();
    return 0;
}

// KEYBOARD SIMULATOR

void* control_listener(void* arg) {
    unsigned char* leds = (unsigned char*)arg;
    int prev_state = LED_OFF;

    int ctrl_cmd_fd = open("ctrl_cmd_pipe", O_RDONLY);
    int ctrl_ack_fd = open("ctrl_ack_pipe", O_WRONLY);

    if (ctrl_cmd_fd < 0 || ctrl_ack_fd < 0) {
        perror("keyboard: control pipe open failed");
        exit(1);
    }

    while (1) {
        char cmd;
        ssize_t n = read(ctrl_cmd_fd, &cmd, 1);
        if (n <= 0) break;

        if (cmd == 'C') {
            int curr = *leds;
            if (curr != prev_state) {
                if (curr == LED_ON) printf("ON ");
                else printf("OFF ");
                fflush(stdout);
            }
            
            prev_state = curr;
            // Just acknowledge either way
            write(ctrl_ack_fd, "A", 1);
        }
    }
    
    close(ctrl_cmd_fd);
    close(ctrl_ack_fd);
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        exit(1);
    }

    // Create pipes (simulate endpoints)
    unlink("int_pipe");
    unlink("ctrl_cmd_pipe");
    unlink("ctrl_ack_pipe");
    
    mkfifo("int_pipe", 0666);
    mkfifo("ctrl_cmd_pipe", 0666);
    mkfifo("ctrl_ack_pipe", 0666);
    
    // Create shared memory for termination flag
    int term_shm_fd = shm_open(TERMINATE_SHM, O_CREAT | O_RDWR, 0666);
    if (term_shm_fd == -1) {
        perror("Failed to create terminate shared memory");
        exit(1);
    }
    
    ftruncate(term_shm_fd, 1);
    unsigned char* terminate_flag = mmap(0, 1, PROT_READ | PROT_WRITE, MAP_SHARED, term_shm_fd, 0);
    close(term_shm_fd);
    
    if (terminate_flag == MAP_FAILED) {
        perror("Failed to map terminate shared memory");
        exit(1);
    }
    
    *terminate_flag = 0; // Initially not terminated
    
    // Create shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to create LED shared memory");
        munmap(terminate_flag, 1);
        shm_unlink(TERMINATE_SHM);
        exit(1);
    }
    
    ftruncate(shm_fd, LED_BUF_SIZE);
    unsigned char* leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (leds == MAP_FAILED) {
        perror("keyboard: mmap failed");
        munmap(terminate_flag, 1);
        shm_unlink(TERMINATE_SHM);
        shm_unlink(SHM_NAME);
        exit(1);
    }

    *leds = LED_OFF; // Initially OFF
    
    // Fork to create driver and keyboard processes
    pid_t pid = fork();
    if (pid < 0) {
        perror("Fork failed");
        munmap(leds, LED_BUF_SIZE);
        munmap(terminate_flag, 1);
        shm_unlink(SHM_NAME);
        shm_unlink(TERMINATE_SHM);
        exit(1);
    }
    
    if (pid == 0) {
        // Child process - run driver
        munmap(leds, LED_BUF_SIZE);
        munmap(terminate_flag, 1);
        return driver();
    }
    
    // Parent process - simulate keyboard
    int int_pipe_fd = open("int_pipe", O_WRONLY);
    if (int_pipe_fd < 0) {
        perror("keyboard: can't open int_pipe");
        kill(pid, SIGTERM);
        munmap(leds, LED_BUF_SIZE);
        munmap(terminate_flag, 1);
        shm_unlink(SHM_NAME);
        shm_unlink(TERMINATE_SHM);
        exit(1);
    }

    // Start LED control listener thread
    pthread_t ctrl_thread;
    pthread_create(&ctrl_thread, NULL, control_listener, leds);

    // Read input file
    FILE* file = fopen(argv[1], "r");
    if (!file) {
        perror("keyboard: can't open input file");
        close(int_pipe_fd);
        *terminate_flag = 1;  // Signal driver to terminate
        pthread_cancel(ctrl_thread);
        pthread_join(ctrl_thread, NULL);
        kill(pid, SIGTERM);
        munmap(leds, LED_BUF_SIZE);
        munmap(terminate_flag, 1);
        shm_unlink(SHM_NAME);
        shm_unlink(TERMINATE_SHM);
        exit(1);
    }

    char ch;
    while ((ch = fgetc(file)) != EOF) {
        write(int_pipe_fd, &ch, 1);
        usleep(20000); // simulate polling delay (20ms)
    }

    // Send end-of-input marker
    write(int_pipe_fd, &(char){END_OF_INPUT}, 1);
    
    fclose(file);
    close(int_pipe_fd);
    
    // Set termination flag
    *terminate_flag = 1;
    
    // Wait for driver to clean up
    int status;
    waitpid(pid, &status, 0);
    
    // Clean up control thread
    pthread_cancel(ctrl_thread);
    pthread_join(ctrl_thread, NULL);
    
    // Clean up resources
    munmap(leds, LED_BUF_SIZE);
    munmap(terminate_flag, 1);
    shm_unlink(SHM_NAME);
    shm_unlink(TERMINATE_SHM);
    
    unlink("int_pipe");
    unlink("ctrl_cmd_pipe");
    unlink("ctrl_ack_pipe");
    
    printf("\n");
    return 0;
}
