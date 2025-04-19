all: keyboard

keyboard: keyboard.c
	gcc -o keyboard keyboard.c -lpthread

clean:
	rm -f keyboard
	rm -f int_pipe ctrl_cmd_pipe ctrl_ack_pipe
	rm -f /dev/shm/led_shm