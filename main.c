#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>

#include "constants.h"
#include "operations.h"
#include "parser.h"
#include "auxiliar.h"

void process_args(int argc, char *argv[]);
unsigned int launch_processes(DIR* dir);
int child_main(struct dirent *dirent);
int process_file(struct dirent *dirent);
void handle_file(int input_no, int output_no);
int handle_command(enum Command cmd, int input_no, int output_no);

unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
unsigned int max_proc = MAX_PROC, max_thread = MAX_THREADS;

int main(int argc, char *argv[])
{
  process_args(argc, argv);

  if (ems_init(state_access_delay_ms))
  {
    fprintf(stderr, "Failed to initialize EMS\n");
    return FAILURE;
  }

  // Fetch file list
  DIR *dir = opendir("jobs");

  if (!dir)
  {
    fprintf(stderr, "Failed to open job directory\n");
    exit(EXIT_FAILURE);
  }

  unsigned int processCount = launch_processes(dir);

  //Wait for all processes
  while (processCount)
  {
    wait(NULL);
    processCount--;
  }

  ems_terminate();
  closedir(dir);
  return 0;
}

void process_args(int argc, char *argv[])
{
  if (argc > 3)
  {
    char *endptr;
    unsigned long int delay = strtoul(argv[3], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX)
    {
      fprintf(stderr, "Invalid delay value or value too large\n");
      exit(EXIT_FAILURE);
    }

    state_access_delay_ms = (unsigned int)delay;
  }
  if(argc > 2){
    char *endptr;
    unsigned long int max_threads = strtoul(argv[2], &endptr, 10);

    if (*endptr != '\0' || max_threads > UINT_MAX)
    {
      fprintf(stderr, "Invalid maximum threads count value or value too large\n");
      exit(EXIT_FAILURE);
    }

    max_thread = (unsigned int)max_threads;
  }
  if (argc > 1)
  {
    char *endptr;
    unsigned long int proc_count = strtoul(argv[1], &endptr, 10);

    if (*endptr != '\0' || proc_count > UINT_MAX)
    {
      fprintf(stderr, "Invalid maximum process count value or value too large\n");
      exit(EXIT_FAILURE);
    }

    max_proc = (unsigned int)proc_count;
  }
}

unsigned int launch_processes(DIR* dir)
{
  unsigned int processCount;
  struct dirent *dirent;

  while ((dirent = readdir(dir)) != NULL)
  {
    pid_t pid = fork();
    if (pid < 0)
    {
      fprintf(stderr, "Fork failed.\n");
      //TODO: Crash or error
    }
    else if (pid == 0)
    {
      int exitCode = child_main(dirent);
      exit(exitCode);
    }
    else
    {
      // Parent process
      processCount++;
      if (processCount >= max_proc)
      {
        // Wait for a child process to finish before forking another
        wait(NULL);
        processCount--;
      }
    }
  }

  return processCount;
}

int child_main(struct dirent *dirent)
{
  int verify = process_file(dirent);
  if (verify == SUCESS)
    return EXIT_SUCCESS;
  return EXIT_FAILURE;
}

int process_file(struct dirent *dirent)
{
  // Could be done later
  char relativePath[1024]; // FIXME: Better size?
  strcpy(relativePath, "jobs");
  pathCombine(relativePath, dirent->d_name);

  // check extension
  char *ext = strstr(relativePath, ".jobs");
  if (ext != &relativePath[strlen(relativePath) - 5])
  {
    return FAILURE;
  }

  int input_no = open(relativePath, O_RDONLY);

  if (input_no == -1)
  {
    // TODO: Handle error
  }

  strcpy(ext, ".out");
  int output_no = open(relativePath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  handle_file(input_no, output_no);
  close(input_no);
  close(output_no);
  return SUCESS;
}

void handle_file(int input_no, int output_no)
{
  while (1)
  {
    enum Command cmd = get_next(input_no);

    if (cmd == CMD_BARRIER)
    {
      //Send exit signal to all threads
    }
    else if (cmd == CMD_WAIT)
    {
      //If global wait, must send some signal to all threads
      //If not, can send to corresponding thread
    }
    else
    {
      if (handle_command(cmd, input_no, output_no))
        break;
    }
  }
}

int handle_command(enum Command cmd, int input_no, int output_no)
{
  unsigned int event_id, delay;
  size_t num_rows, num_columns, num_coords;
  size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

  switch (cmd)
  {
  case CMD_CREATE:
    if (parse_create(input_no, &event_id, &num_rows, &num_columns) != 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    if (ems_create(event_id, num_rows, num_columns))
    {
      fprintf(stderr, "Failed to create event\n");
    }

    break;

  case CMD_RESERVE:
    num_coords = parse_reserve(input_no, MAX_RESERVATION_SIZE, &event_id, xs, ys);

    if (num_coords == 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    if (ems_reserve(event_id, num_coords, xs, ys))
    {
      fprintf(stderr, "Failed to reserve seats\n");
    }

    break;

  case CMD_SHOW:
    if (parse_show(input_no, &event_id) != 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    if (ems_show(event_id, output_no))
    {
      fprintf(stderr, "Failed to show event\n");
    }

    break;

  case CMD_LIST_EVENTS:
    if (ems_list_events(output_no))
    {
      fprintf(stderr, "Failed to list events\n");
    }

    break;

  case CMD_WAIT:
    if (parse_wait(input_no, &delay, NULL) == -1)
    { // thread_id is not implemented
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    if (delay > 0)
    {
      printf("Waiting...\n");
      ems_wait(delay);
    }

    break;

  case CMD_INVALID:
    fprintf(stderr, "Invalid command. See HELP for usage\n");
    break;

  case CMD_HELP:
    printf(
        "Available commands:\n"
        "  CREATE <event_id> <num_rows> <num_columns>\n"
        "  RESERVE <event_id> [(<x1>,<y1>) (<x2>,<y2>) ...]\n"
        "  SHOW <event_id>\n"
        "  LIST\n"
        "  WAIT <delay_ms> [thread_id]\n" // thread_id is not implemented
        "  BARRIER\n"                     // Handled separately
        "  HELP\n");

    break;

  case CMD_BARRIER: // Handled separately
  case CMD_EMPTY:
    break;

  case EOC:
    // TODO: Write events to output file
    return 1;
  }

  return 0;
}
