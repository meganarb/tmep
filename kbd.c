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

#define LED_BUF_SIZE 1
#define SHM_NAME "/led_shm"

#define NO_EVENT        '#'
#define CAPSLOCK_PRESS  '@'
#define CAPSLOCK_RELEASE '&'

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
    void* context;
};

struct usb_kbd {
    struct input_dev* dev;

    int int_ep_fd;     // for reading from keyboard (interrupt endpoint)
    int ctrl_cmd_fd;   // for writing LED control commands
    int ctrl_ack_fd;   // for reading ACKs

    unsigned char* leds;
    pthread_mutex_t leds_lock;

    // URBs for the endpoints
    struct urb* int_urb;
    struct urb* led_urb;
};

// Global variables
usb_kbd kbd;
int capslock_state = 0;

// Function prototypes
void usb_submit_urb(urb* urb);
void* usb_kbd_irq(void* arg);
void* usb_kbd_led(void* arg);
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value);
void usb_kbd_event(struct input_dev* dev);
void print_char(char ch);
int usb_kbd_open(void);

// Print character with capslock applied if needed
void print_char(char ch) {
    if (capslock_state && ch >= 'a' && ch <= 'z')
        ch = ch - 'a' + 'A';
    printf("%c", ch);
    fflush(stdout);
}

// Submit an URB (Universal Request Block) to start or continue endpoint handling
void usb_submit_urb(urb* urb) {
    if (!urb || urb->active) return;

    urb->active = 1;

    if (urb->endpoint_type == 0) { // Interrupt endpoint
        pthread_create(&urb->thread, NULL, usb_kbd_irq, urb);
    }
    else { // Control endpoint
        pthread_create(&urb->thread, NULL, usb_kbd_led, urb);
    }

    pthread_detach(urb->thread);
}

// Interrupt endpoint handler - processes key events
void* usb_kbd_irq(void* arg) {
    urb* irq_urb = (urb*)arg;
    usb_kbd* kbd_ptr = (usb_kbd*)irq_urb->context;
    irq_urb->active = 0;

    char ch;
    ssize_t n = read(kbd_ptr->int_ep_fd, &ch, 1);

    if (n > 0 && ch != NO_EVENT) {
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

    if (n > 0) {
        // Re-submit URB to continue polling
        usb_submit_urb(irq_urb);
    }

    return NULL;
}

// Control endpoint handler - handles LED state changes
void* usb_kbd_led(void* arg) {
    urb* led_urb = (urb*)arg;
    usb_kbd* kbd_ptr = (usb_kbd*)led_urb->context;
    led_urb->active = 0;

    // Send control command
    write(kbd_ptr->ctrl_cmd_fd, "C", 1);

    // Wait for ACK
    char ack;
    read(kbd_ptr->ctrl_ack_fd, &ack, 1);

    if (ack == 'A') {
        // If needed, we could do something with the acknowledgment
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

// Initialize and open the USB keyboard device
int usb_kbd_open(void) {
    // Set up the input device
    input_dev* dev = malloc(sizeof(input_dev));
    if (!dev) return -1;

    dev->event = usb_kbd_event;
    dev->led = LED_OFF;
    kbd.dev = dev;

    // Initialize the mutex
    pthread_mutex_init(&kbd.leds_lock, NULL);

    // Open the pipes
    kbd.int_ep_fd = open("int_pipe", O_RDONLY);
    kbd.ctrl_cmd_fd = open("ctrl_cmd_pipe", O_WRONLY);
    kbd.ctrl_ack_fd = open("ctrl_ack_pipe", O_RDONLY);

    if (kbd.int_ep_fd < 0 || kbd.ctrl_cmd_fd < 0 || kbd.ctrl_ack_fd < 0) {
        perror("pipe open failed");
        free(dev);
        return -1;
    }

    // Set up shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        free(dev);
        return -1;
    }

    kbd.leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (kbd.leds == MAP_FAILED) {
        perror("mmap failed");
        free(dev);
        return -1;
    }

    // Create URBs
    kbd.int_urb = malloc(sizeof(urb));
    kbd.led_urb = malloc(sizeof(urb));

    if (!kbd.int_urb || !kbd.led_urb) {
        perror("Failed to allocate URBs");
        free(dev);
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

// Driver function - now calls usb_kbd_open
int driver() {
    if (usb_kbd_open() < 0) {
        fprintf(stderr, "Failed to open USB keyboard\n");
        return -1;
    }

    // Main loop - just wait
    while (1) {
        sleep(1);
    }

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
        if (read(ctrl_cmd_fd, &cmd, 1) <= 0) break;

        if (cmd == 'C') {
            int curr = *leds;
            if (curr != prev_state) {
                if (curr == LED_ON) printf("ON ");
                else printf("OFF ");
            }

            prev_state = curr;
            // Just acknowledge either way
            write(ctrl_ack_fd, "A", 1);
        }
    }
    printf("\n");

    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        exit(1);
    }

    // Create pipes (simulate endpoints)
    mkfifo("int_pipe", 0666);
    mkfifo("ctrl_cmd_pipe", 0666);
    mkfifo("ctrl_ack_pipe", 0666);

    // Fork to create driver and keyboard processes
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) return driver();

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
        usleep(20000); // simulate polling delay (20ms)
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