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

#define NO_EVENT '#'
#define CAPSLOCK_PRESS '@'
#define CAPSLOCK_RELEASE '&'

#define LED_ON 1
#define LED_OFF 0

#define SHM_NAME "/led_shm"

struct input_dev {
    void (*event)(struct input_dev* dev);
    int led;
};

struct usb_kbd {
    struct input_dev* dev;

    int int_ep_fd; // interrupt endpoint
    int ctrl_cmd_fd; // control endpoint
    int ctrl_ack_fd; // ack

    unsigned char* leds;
    pthread_mutex_t leds_lock;
};

typedef struct usb_kbd usb_kbd;
typedef struct input_dev input_dev;

void input_report_key(struct usb_kbd* kbd, unsigned int code, int value);

usb_kbd kbd;
int capslock_state = 0;

void print_char(char ch) {
    if (capslock_state && ch >= 'a' && ch <= 'z')
        ch = ch - 'a' + 'A';
    printf("%c", ch);
    fflush(stdout);
}

// input event callback
void usb_kbd_event(struct input_dev* dev_ptr) {
    if (dev_ptr->led == LED_ON && !capslock_state) {
        capslock_state = 1;
    }
    else if (dev_ptr->led == LED_OFF && capslock_state) {
        capslock_state = 0;
    }

    // update led
    *(kbd.leds) = dev_ptr->led ? LED_ON : LED_OFF;
    // control command
    write(kbd.ctrl_cmd_fd, "C", 1);
    // wait for ack
    char ack;
    read(kbd.ctrl_ack_fd, &ack, 1);

}

// irq handler
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

// key events
void input_report_key(struct usb_kbd* kbd, unsigned int code, int value) {
    if (code == CAPSLOCK_PRESS || code == CAPSLOCK_RELEASE) {
        kbd->dev->led = value;
        pthread_t tid;
        pthread_create(&tid, NULL, (void*)kbd->dev->event, kbd->dev);
        pthread_detach(tid);
    }
}

int driver() { // covers driver main, usb_kbd_open, usb_submit_urb

    int int_pipe[2], ctrl_cmd_pipe[2], ctrl_ack_pipe[2];
    kbd.int_ep_fd = open("int_pipe", O_RDONLY);
    kbd.ctrl_cmd_fd = open("ctrl_cmd_pipe", O_WRONLY);
    kbd.ctrl_ack_fd = open("ctrl_ack_pipe", O_RDONLY);

    if (kbd.int_ep_fd < 0 || kbd.ctrl_cmd_fd < 0 || kbd.ctrl_ack_fd < 0) {
        perror("pipe open failed");
        exit(1);
    }

    // shared mem for led :D
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

    pthread_mutex_init(&kbd.leds_lock, NULL);
    input_dev* dev = malloc(sizeof(input_dev));
    dev->event = usb_kbd_event;
    dev->led = LED_OFF;
    kbd.dev = dev;

    // usb_kbd_open
    while (1) {
        char ch;
        ssize_t n = read(kbd.int_ep_fd, &ch, 1);
        if (n <= 0) break;

        if (ch == NO_EVENT) continue;

        char* pch = malloc(1);
        *pch = ch;
        pthread_t irq_thread;
        pthread_create(&irq_thread, NULL, usb_kbd_irq, pch);
        pthread_detach(irq_thread);
    }
    //printf("\n"); // if there is no newline at end of file, uncomment this :)
    
    return 0;
}

void* control_listener(void* arg) {
    unsigned char* leds = (unsigned char*)arg;
    int prev_state = LED_OFF;

    int ctrl_cmd_fd = open("ctrl_cmd_pipe", O_RDONLY);
    int ctrl_ack_fd = open("ctrl_ack_pipe", O_WRONLY);

    if (ctrl_cmd_fd < 0 || ctrl_ack_fd < 0) {
        perror("failed to open pipe");
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
            // send ack
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

    // creating pipes for the endpoints
    mkfifo("int_pipe", 0666);
    mkfifo("ctrl_cmd_pipe", 0666);
    mkfifo("ctrl_ack_pipe", 0666);
    
    // start separate driver process
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) return driver();

    int int_pipe_fd = open("int_pipe", O_WRONLY);
    if (int_pipe_fd < 0) {
        perror("can't open pipe int_pipe");
        exit(1);
    }

    // shared mem led buf
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, LED_BUF_SIZE);
    unsigned char* leds = mmap(0, LED_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (leds == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    *leds = LED_OFF;

    pthread_t ctrl_thread;
    pthread_create(&ctrl_thread, NULL, control_listener, leds);

    // getting input from file
    FILE* file = fopen(argv[1], "r");
    if (!file) {
        perror("unable to open input file");
        exit(1);
    }

    char ch;
    while ((ch = fgetc(file)) != EOF) {
        write(int_pipe_fd, &ch, 1);
        usleep(20000); // delay, letters get jumbled otherwise :(
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
