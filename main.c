#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>

#include "constants.h"
#include "operations.h"
#include "parser.h"
#include "auxiliar.h"

struct thread_info
{
  unsigned int index;
  pthread_t id;
  int input_no, output_no;
  unsigned int line;
  pthread_mutex_t line_lock;
} *thread_infos;
unsigned int used_threads = 0;

void *process_args(int argc, char *argv[]);
unsigned int launch_processes(DIR *dir);
int child_main(struct dirent *dirent);
int process_file(struct dirent *dirent);
void handle_file(int input_no, int output_no);
int handle_command(enum Command cmd, int input_no, int output_no, struct thread_info *my_info);
void lockAll();
void unlockAll();
void *thread_main(void *argument);

unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
unsigned int max_proc = MAX_PROC, max_thread = MAX_THREADS;
char *glob_dirPath;

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

  // Wait for all processes
  while (processCount)
  {
    wait(NULL);
    processCount--;
  }

  ems_terminate();
  closedir(dir);
  return 0;
}

void *process_args(int argc, char *argv[])
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
  if (argc > 2)
  {
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

  return NULL;
}

unsigned int launch_processes(DIR *dir)
{
  unsigned int processCount;
  struct dirent *dirent;

  while ((dirent = readdir(dir)) != NULL)
  {
    pid_t pid = fork();
    if (pid < 0)
    {
      fprintf(stderr, "Fork failed.\n");
      exit(EXIT_FAILURE);
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
    exit(EXIT_FAILURE);

  strcpy(ext, ".out");
  int output_no = open(relativePath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

  free(thread_infos);
  handle_file(input_no, output_no);
  close(input_no);
  close(output_no);
  return SUCESS;
}

void handle_file(int input_no, int output_no)
{
  char ch;
  unsigned int line_count = 0;
  while (read(input_no, &ch, 1) != 0)
  {
    if (ch == '\n')
    {
      line_count++;
    }
  }
  lseek(input_no, 0, SEEK_SET); // Reset file descriptor to the start

  used_threads = line_count > max_thread ? max_thread : line_count; //TODO: Verify this
  thread_infos = malloc(used_threads * sizeof(pthread_t));

  // duplicate input_no with dup
  for (unsigned int i = 0; i < used_threads; i++)
  {
    thread_infos[i].index = i;
    thread_infos[i].input_no = dup(input_no);
    thread_infos[i].output_no = dup(output_no);
    thread_infos[i].line = 0;
    pthread_mutex_init(&thread_infos[i].line_lock, NULL);
    
    if (pthread_create(&thread_infos[i].id, NULL, thread_main, &thread_infos[i]))
    {
      fprintf(stderr, "Failed to create thread\n");
      exit(EXIT_FAILURE);
    }
  }
}
void lockAll()
{
  sleep(1); //TODO: Make better
  for (unsigned int i = 0; i < used_threads; i++)
  {
    if (thread_infos[i].id==pthread_self()) continue;
    pthread_mutex_lock(&thread_infos[i].line_lock);
  }
}
void unlockAll()
{
  for (unsigned int i = 0; i < used_threads; i++)
  {
    if(thread_infos[i].id==pthread_self()) continue;
    pthread_mutex_unlock(&thread_infos[i].line_lock);
  }
}
void *thread_main(void *argument)
{
  struct thread_info *arg = (struct thread_info *)argument;
  int should_exit = 0;

  while (!should_exit)
  {
    enum Command cmd = get_next(arg->input_no);

    if (cmd == CMD_WAIT || cmd == CMD_BARRIER)
    {
      // must always be checked for execution
      should_exit = handle_command(cmd, arg->input_no, arg->output_no, arg);
    }
    else
    {
      // only execute if on assigned lines
      //local reads to line are safe, only I write to line
      if (arg->line % used_threads == (arg->index - 1))
        should_exit = handle_command(cmd, arg->input_no, arg->output_no, arg);
    }

    pthread_mutex_lock(&arg->line_lock);
    arg->line++;
    pthread_mutex_unlock(&arg->line_lock);
  }

  return NULL;
}

int handle_command(enum Command cmd, int input_no, int output_no, struct thread_info *my_info)
{
  unsigned int event_id, delay, thread_id;
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

  case CMD_LIST_EVENTS:
    lockAll();
    ems_list_events(output_no);
    unlockAll();
  break;
  
  case CMD_SHOW:
    if (parse_show(input_no, &event_id) != 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }
    lockAll();
    if (ems_show(event_id, output_no))
    {
      fprintf(stderr, "Failed to show event\n");
    }
    unlockAll();
    break;

  case CMD_WAIT:
    if (parse_wait(input_no, &delay, &thread_id) == -1)
    { // thread_id is not implemented
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }
    if (delay <= 0)
      break;
    if (thread_id == 0)
    {
      printf("Waiting...\n");
      ems_wait(delay);
    }
    else if (thread_id-1 == my_info->index)
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
        "  WAIT <delay_ms> [thread_id]\n"
        "  BARRIER\n"
        "  HELP\n");

    break;

  case CMD_BARRIER:
    char barrier_can_continue = 0;
    while (!barrier_can_continue)
    {
      barrier_can_continue = 1;

      for (unsigned int i = 0; i < used_threads; i++)
      {
        if (i == my_info->index) continue;
        pthread_mutex_lock(&thread_infos[i].line_lock);
        unsigned int other_line = thread_infos[i].line;
        pthread_mutex_unlock(&thread_infos[i].line_lock);
        if (other_line != my_info->line)
        {
          barrier_can_continue = 0;
          break;
        }
      }
      //sleep to prevent spamming mutex locks
      if (!barrier_can_continue)
        ems_wait(2);
    }
    break;

  case CMD_EMPTY:
    break;

  case EOC:
    return 1;
  }
  
  return 0;
}
