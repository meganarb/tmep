#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LED_BUF_SIZE 1

#define NO_EVENT        '#'
#define CAPSLOCK_PRESS  '@'
#define CAPSLOCK_RELEASE '&'

#define LED_ON  1
#define LED_OFF 0

#define URB_TYPE_INT  1
#define URB_TYPE_CTRL 2

// Forward declarations
struct usb_kbd;
struct input_dev;
struct urb;

// Callback function type for URB completion
typedef void (*urb_complete_t)(struct urb *);

// URB structure for USB requests
struct urb {
    int type;                 // URB_TYPE_INT or URB_TYPE_CTRL
    void *transfer_buffer;    // Buffer for data transfer
    int transfer_buffer_length; // Length of the buffer
    urb_complete_t complete;  // Completion handler
    void *context;            // Context for the completion handler
    int pipe;                 // Pipe to use for this URB
    int status;               // Status of the URB
    int actual_length;        // Actual length of data transferred
    pthread_t thread;         // Thread handling this URB
    int active;               // Whether this URB is active
};

// Input device structure
struct input_dev {
    void (*event)(struct input_dev* dev);  // Event callback
    int led;                               // LED state
};

// USB keyboard device structure
struct usb_kbd {
    struct input_dev* dev;            // Input device
    
    int int_ep_fd;                    // Interrupt endpoint file descriptor
    int ctrl_cmd_fd;                  // Control command file descriptor
    int ctrl_ack_fd;                  // Control acknowledgment file descriptor
    
    unsigned char* leds;              // LED buffer
    
    pthread_mutex_t leds_lock;        // Lock for LED buffer
    
    struct urb *irq_urb;              // URB for interrupt endpoint
    struct urb *led_urb;              // URB for LED control
    
    int open;                         // Whether the keyboard is open
};

typedef struct usb_kbd usb_kbd;
typedef struct input_dev input_dev;
typedef struct urb urb;

// Function declarations
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value);
int usb_submit_urb(struct urb *urb);
void usb_kbd_irq(struct urb *urb);
void usb_kbd_led(struct urb *urb);
int usb_kbd_open(struct usb_kbd *kbd);

// Shared memory name
#define SHM_NAME "/led_shm"

// Global variables
usb_kbd kbd;
int capslock_state = 0;

// Print character with capslock handling
void print_char(char ch) {
    if (capslock_state && ch >= 'a' && ch <= 'z')
        ch = ch - 'a' + 'A';
    else if (!capslock_state && ch >= 'A' && ch <= 'Z')
        ch = ch - 'A' + 'a';

    printf("%c", ch);
    fflush(stdout);
}

// Input device event callback
void usb_kbd_event(struct input_dev* dev_ptr) {
    struct usb_kbd *kbd = container_of(dev_ptr, struct usb_kbd, dev);
    
    // Update LED state
    pthread_mutex_lock(&kbd->leds_lock);
    *(kbd->leds) = dev_ptr->led;
    pthread_mutex_unlock(&kbd->leds_lock);
    
    // Send control command
    write(kbd.ctrl_cmd_fd, "C", 1);
    
    // Wait for ACK
    char ack;
    read(kbd.ctrl_ack_fd, &ack, 1);

    if (dev_ptr->led == LED_ON && !capslock_state) {
        capslock_state = 1;
        printf("\nON\n");
    }
    else if (dev_ptr->led == LED_OFF && capslock_state) {
        capslock_state = 0;
        printf("\nOFF\n");
    }
    
    // Submit a new LED URB to maintain the control endpoint
    if (kbd->led_urb) {
        usb_submit_urb(kbd->led_urb);
    }
}

// Container_of macro to get the containing structure
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// Interrupt endpoint URB completion handler
void usb_kbd_irq(struct urb *urb) {
    if (urb->status < 0) return;
    
    struct usb_kbd *kbd = (struct usb_kbd *)urb->context;
    unsigned char *data = urb->transfer_buffer;
    char ch = *data;
    
    if (ch == NO_EVENT) {
        // Resubmit the URB to continue polling
        usb_submit_urb(urb);
        return;
    }
    
    if (ch == CAPSLOCK_PRESS) {
        input_report_key(kbd, CAPSLOCK_PRESS, LED_ON);
    }
    else if (ch == CAPSLOCK_RELEASE) {
        input_report_key(kbd, CAPSLOCK_RELEASE, LED_OFF);
    }
    else {
        print_char(ch);
    }
    
    // Resubmit the URB to continue polling
    usb_submit_urb(urb);
}

// Control endpoint (LED) URB completion handler
void usb_kbd_led(struct urb *urb) {
    struct usb_kbd *kbd = (struct usb_kbd *)urb->context;
    
    // The LED command is already sent in usb_kbd_event
    // This function is here to handle any additional LED-related processing
    
    // No need to resubmit the URB here, it's done in usb_kbd_event
}

// Report a key event to the input subsystem
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value) {
    if (code == CAPSLOCK_PRESS || code == CAPSLOCK_RELEASE) {
        kbd->dev->led = value;
        
        // Create a new thread for event handling
        pthread_t event_thread;
        pthread_create(&event_thread, NULL, (void* (*)(void*))kbd->dev->event, kbd->dev);
        pthread_detach(event_thread);
    }
}

// Submit a URB to USB core
int usb_submit_urb(struct urb *urb) {
    if (!urb || urb->active) return -1;
    
    urb->active = 1;
    
    // For first-time submission, create a thread to handle this endpoint
    if (urb->thread == 0) {
        if (urb->type == URB_TYPE_INT) {
            // Create interrupt endpoint thread
            pthread_create(&urb->thread, NULL, (void *(*)(void *))urb_int_thread, urb);
            pthread_detach(urb->thread);
        } else if (urb->type == URB_TYPE_CTRL) {
            // Create control endpoint thread
            pthread_create(&urb->thread, NULL, (void *(*)(void *))urb_ctrl_thread, urb);
            pthread_detach(urb->thread);
        }
    }
    
    return 0;
}

// Interrupt endpoint thread
void *urb_int_thread(struct urb *urb) {
    struct usb_kbd *kbd = (struct usb_kbd *)urb->context;
    
    while (kbd->open) {
        // Read from interrupt endpoint
        char ch;
        ssize_t n = read(kbd->int_ep_fd, &ch, 1);
        if (n <= 0) break;
        
        // Store the data and call completion handler in a new thread
        *(char *)urb->transfer_buffer = ch;
        urb->actual_length = 1;
        
        pthread_t irq_thread;
        pthread_create(&irq_thread, NULL, (void *(*)(void *))urb->complete, urb);
        pthread_detach(irq_thread);
        
        // Simulate polling delay
        usleep(10000); // 10ms
    }
    
    return NULL;
}

// Control endpoint thread
void *urb_ctrl_thread(struct urb *urb) {
    struct usb_kbd *kbd = (struct usb_kbd *)urb->context;
    
    while (kbd->open) {
        // Wait for control commands to be processed
        // The actual sending happens in usb_kbd_event
        
        // Call completion handler in a new thread when needed
        pthread_t led_thread;
        pthread_create(&led_thread, NULL, (void *(*)(void *))urb->complete, urb);
        pthread_detach(led_thread);
        
        // Wait before checking again
        usleep(50000); // 50ms
    }
    
    return NULL;
}

// Open USB keyboard device
int usb_kbd_open(struct usb_kbd *kbd) {
    // Initialize input device
    input_dev* dev = malloc(sizeof(input_dev));
    if (!dev) return -1;
    
    dev->event = usb_kbd_event;
    dev->led = LED_OFF;
    kbd->dev = dev;
    
    // Initialize locks
    pthread_mutex_init(&kbd->leds_lock, NULL);
    
    // Open pipes
    kbd->int_ep_fd = open("int_pipe", O_RDONLY);
    kbd->ctrl_cmd_fd = open("ctrl_cmd_pipe", O_WRONLY);
    kbd->ctrl_ack_fd = open("ctrl_ack_pipe", O_RDONLY);
    
    if (kbd->int_ep_fd < 0 || kbd->ctrl_cmd_fd < 0 || kbd->ctrl_ack_fd < 0) {
        perror("pipe open failed");
        return -1;
    }
    
    // Set up shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        return -1;
    }
    
    kbd->leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (kbd->leds == MAP_FAILED) {
        perror("mmap failed");
        return -1;
    }
    
    // Create URBs
    // Interrupt URB
    kbd->irq_urb = malloc(sizeof(struct urb));
    if (!kbd->irq_urb) return -1;
    
    kbd->irq_urb->type = URB_TYPE_INT;
    kbd->irq_urb->transfer_buffer = malloc(1);
    kbd->irq_urb->transfer_buffer_length = 1;
    kbd->irq_urb->complete = usb_kbd_irq;
    kbd->irq_urb->context = kbd;
    kbd->irq_urb->active = 0;
    kbd->irq_urb->thread = 0;
    
    // LED URB
    kbd->led_urb = malloc(sizeof(struct urb));
    if (!kbd->led_urb) return -1;
    
    kbd->led_urb->type = URB_TYPE_CTRL;
    kbd->led_urb->transfer_buffer = malloc(1);
    kbd->led_urb->transfer_buffer_length = 1;
    kbd->led_urb->complete = usb_kbd_led;
    kbd->led_urb->context = kbd;
    kbd->led_urb->active = 0;
    kbd->led_urb->thread = 0;
    
    // Set keyboard as open
    kbd->open = 1;
    
    // Submit URBs
    usb_submit_urb(kbd->irq_urb);
    usb_submit_urb(kbd->led_urb);
    
    printf("Driver started. Listening to keyboard input...\n");
    
    return 0;
}

// Driver entry point
int driver() {
    // Call usb_kbd_open to initialize the driver
    if (usb_kbd_open(&kbd) < 0) {
        fprintf(stderr, "Failed to open USB keyboard\n");
        exit(1);
    }
    
    // Wait for keyboard to be closed
    while (kbd.open) {
        sleep(1);
    }
    
    printf("\nDriver shutting down.\n");
    return 0;
}

// Control listener for keyboard process
void* control_listener(void* arg) {
    unsigned char* leds = (unsigned char*)arg;

    int ctrl_cmd_fd = open("ctrl_cmd_pipe", O_RDONLY);
    int ctrl_ack_fd = open("ctrl_ack_pipe", O_WRONLY);

    if (ctrl_cmd_fd < 0 || ctrl_ack_fd < 0) {
        perror("keyboard: control pipe open failed");
        exit(1);
    }

    while (1) {
        char cmd;
        if (read(ctrl_cmd_fd, &cmd, 1) <= 0) break;

        if (cmd == 'C') {
            if (*leds == LED_ON) {
                capslock_led_state = 1;  // Turn on
            } else {
                capslock_led_state = 0;  // Turn off
            }
            // Acknowledge
            write(ctrl_ack_fd, "A", 1);
        }
    }

    return NULL;
}

// Main function
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        exit(1);
    }

    // Fork to create driver process
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) return driver();

    // Keyboard process continues here
    
    // Create pipes (simulate endpoints)
    mkfifo("int_pipe", 0666);
    mkfifo("ctrl_cmd_pipe", 0666);
    mkfifo("ctrl_ack_pipe", 0666);

    int int_pipe_fd = open("int_pipe", O_WRONLY);
    if (int_pipe_fd < 0) {
        perror("keyboard: can't open int_pipe");
        exit(1);
    }

    // Create shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, LED_BUF_SIZE);
    unsigned char* leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (leds == MAP_FAILED) {
        perror("keyboard: mmap failed");
        exit(1);
    }

    *leds = LED_OFF; // Initially OFF

    // Start LED control listener thread
    pthread_t ctrl_thread;
    pthread_create(&ctrl_thread, NULL, control_listener, leds);

    // Read input file
    FILE* file = fopen(argv[1], "r");
    if (!file) {
        perror("keyboard: can't open input file");
        exit(1);
    }

    char ch;
    while ((ch = fgetc(file)) != EOF) {
        write(int_pipe_fd, &ch, 1);
        usleep(10000); // simulate polling delay (10ms)
    }

    fclose(file);
    close(int_pipe_fd);
    pthread_join(ctrl_thread, NULL);

    munmap(leds, LED_BUF_SIZE);
    shm_unlink(SHM_NAME);

    unlink("int_pipe");
    unlink("ctrl_cmd_pipe");
    unlink("ctrl_ack_pipe");

    return 0;
}
