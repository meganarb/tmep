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

#define NO_EVENT        '#'
#define CAPSLOCK_PRESS  '@'
#define CAPSLOCK_RELEASE '&'

#define LED_ON  1
#define LED_OFF 0

struct input_dev {
    void (*event)(struct input_dev* dev);
    int led;
};

struct usb_kbd {
    struct input_dev* dev;

    int int_ep_fd;     // for reading from keyboard (interrupt endpoint)
    int ctrl_cmd_fd;   // for writing LED control commands
    int ctrl_ack_fd;   // for reading ACKs

    unsigned char* leds;

    pthread_mutex_t leds_lock;
};

typedef struct usb_kbd usb_kbd;
typedef struct input_dev input_dev;

void input_report_key(struct usb_kbd* kbd, unsigned int code, int value);

// DRIVER

#define SHM_NAME "/led_shm"

usb_kbd kbd;
int capslock_state = 0;
char capslock_msg[1024] = "";

void print_char(char ch) {
    if (capslock_state && ch >= 'a' && ch <= 'z')
        ch = ch - 'a' + 'A';
    if (ch != '\n')
    printf("%c", ch);
    fflush(stdout);
}

// Simulated input_event callback
void usb_kbd_event(struct input_dev* dev_ptr) {
    if (dev_ptr->led == LED_ON && !capslock_state) {
        capslock_state = 1;
    }
    else if (dev_ptr->led == LED_OFF && capslock_state) {
        capslock_state = 0;
    }

    // Write new LED state to shared memory
    *(kbd.leds) = dev_ptr->led ? LED_ON : LED_OFF;
    // Send control command
    write(kbd.ctrl_cmd_fd, "C", 1);

    // Wait for ACK
    char ack;
    read(kbd.ctrl_ack_fd, &ack, 1);

}

// Simulated irq handler
void* usb_kbd_irq(void* arg) {
    char ch = *(char*)arg;
    free(arg);

    if (ch == CAPSLOCK_PRESS) {
        input_report_key(&kbd, CAPSLOCK_PRESS, kbd.dev->led == LED_ON ? LED_OFF : LED_ON);
    }
    else if (ch != CAPSLOCK_RELEASE) {
        print_char(ch);
    }

    return NULL;
}

// Report a key event
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value) {
    if (code == CAPSLOCK_PRESS || code == CAPSLOCK_RELEASE) {
        kbd->dev->led = value;
        
        pthread_t tid;
        pthread_create(&tid, NULL, (void* (*)(void*))kbd->dev->event, kbd->dev);
        pthread_join(tid, NULL);
    }
}

int driver() {
    // Set up pipes
    int int_pipe[2], ctrl_cmd_pipe[2], ctrl_ack_pipe[2];

    // Pipes must be pre-created using dup/exec between processes or use fixed names
    // For now, we'll use known names for FIFO-like behavior

    // Assume these pipes are already created by keyboard process:
    kbd.int_ep_fd = open("int_pipe", O_RDONLY);
    kbd.ctrl_cmd_fd = open("ctrl_cmd_pipe", O_WRONLY);
    kbd.ctrl_ack_fd = open("ctrl_ack_pipe", O_RDONLY);

    if (kbd.int_ep_fd < 0 || kbd.ctrl_cmd_fd < 0 || kbd.ctrl_ack_fd < 0) {
        perror("pipe open failed");
        exit(1);
    }

    // Set up shared memory for LED buffer
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(1);
    }

    kbd.leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (kbd.leds == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    // Init usb_kbd fields
    pthread_mutex_init(&kbd.leds_lock, NULL);
    input_dev* dev = malloc(sizeof(input_dev));
    dev->event = usb_kbd_event;
    dev->led = LED_OFF;
    kbd.dev = dev;

    // Call open (simulated)

    while (1) {
        char ch;
        ssize_t n = read(kbd.int_ep_fd, &ch, 1);
        if (n <= 0) break;

        if (ch == NO_EVENT) continue;

        char* pch = malloc(1);
        *pch = ch;
        pthread_t irq_thread;
        pthread_create(&irq_thread, NULL, usb_kbd_irq, pch);
        pthread_join(irq_thread, NULL);

        // Simulate short delay between polls
        usleep(10000); // 10ms
    }
    printf("\n");
    
    return 0;
}



// KEYBOARD

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
    // These should match the names expected by driver
    mkfifo("int_pipe", 0666);
    mkfifo("ctrl_cmd_pipe", 0666);
    mkfifo("ctrl_ack_pipe", 0666);
    
    // added by me :)
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
        usleep(20000); // simulate polling delay (10ms)
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
