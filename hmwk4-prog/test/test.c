#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) 
{
	if (argc != 3) {
		printf("Usage: test <num cpus> <cpu group>\n");
		return 0;
	}
	
	if (syscall(__NR_sched_set_CPUgroup, atoi(argv[1]), atoi(argv[2])))
		printf("Failed to change CPU groups\n");
	else
		printf("Changed CPU groups\n");

	return 0;
}
