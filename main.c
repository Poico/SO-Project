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
  char path[512];
  unsigned int line;
  pthread_mutex_t line_lock;
  int exit_code;
} *thread_infos;

unsigned int used_threads = 0;

void process_args(int argc, char *argv[]);
unsigned int launch_processes(DIR *dir);
int child_main(struct dirent *dirent);
int process_file(struct dirent *dirent);
void handle_file(char *relativePath);
int handle_command(enum Command cmd, struct thread_info *my_info, int input_no);
void waited_threadsInitialized();
void free_waited_threads();
void thread_exit(struct thread_info *me, int code);
int out_num;
pthread_mutex_t out_lock;
unsigned int *waited_threads;

void *thread_main(void *argument);

unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
unsigned int max_proc = MAX_PROC, max_thread = MAX_THREADS;
char *glob_dirPath;

int main(int argc, char *argv[])
{
  process_args(argc, argv);

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
  free_waited_threads();
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

  // DBG
  printf("Using %d procs, %d threads and %d delay.\n", max_proc, max_thread, state_access_delay_ms);
}

unsigned int launch_processes(DIR *dir)
{
  unsigned int processCount = 0;
  struct dirent *dirent;

  while ((dirent = readdir(dir)) != NULL)
  {
    // DBG
    printf("Forking for file '%s'.\n", dirent->d_name);
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
  if (ems_init(state_access_delay_ms))
  {
    fprintf(stderr, "Failed to initialize EMS\n");
    return FAILURE;
  }
  int verify = process_file(dirent);
  ems_terminate();
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

  // DBG
  printf("Opening file '%s'.\n", relativePath);
  char relativePathCopy[1024];
  strcpy(relativePathCopy, relativePath);
  strcpy(ext, ".out");
  out_num = open(relativePath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  pthread_mutex_init(&out_lock, NULL);

  handle_file(relativePathCopy);

  pthread_mutex_destroy(&out_lock);
  free(thread_infos);
  close(out_num);
  return SUCESS;
}

void handle_file(char *relativePath)
{
  // DBG
  printf("At %s.\n", relativePath);
  int fd = open(relativePath, O_RDONLY);
  if (fd == -1)
  {
    // Handle error
  }

  unsigned int line_count = 0;
  char ch;
  while (read(fd, &ch, 1) > 0)
  {
    if (ch == '\n')
    {
      line_count++;
    }
  }

  close(fd);
  if (line_count < max_thread)
  {
    used_threads = line_count;
  }
  else
  {
    used_threads = max_thread;
  }
  waited_threadsInitialized();

  thread_infos = malloc(used_threads * sizeof(struct thread_info));

  // duplicate input_no with dup
  for (unsigned int i = 0; i < used_threads; i++)
  {
    // DBG
    printf("Launching thread %d.\n", i);

    thread_infos[i].index = i;
    strcpy(thread_infos[i].path, relativePath);
    thread_infos[i].line = 0;
    pthread_mutex_init(&thread_infos[i].line_lock, NULL);
  }

  char finished = 0;
  int *ret_code;
  while (!finished)
  {
    for (unsigned int i = 0; i < used_threads; i++)
    {
      if (pthread_create(&thread_infos[i].id, NULL, thread_main, &thread_infos[i]))
      {
        fprintf(stderr, "Failed to create thread\n");
        exit(EXIT_FAILURE);
      }
    }

    for (unsigned int i = 0; i < used_threads; i++)
    {
      pthread_join(thread_infos[i].id, (void**)&ret_code);
      if (*ret_code == THREAD_FINISHED)
      {
        finished = 1;
      }
    }
  }
}

void *thread_main(void *argument)
{
  struct thread_info *arg = (struct thread_info *)argument;
  int should_exit = 0;
  int input_no = open(arg->path, O_RDONLY);
  // DBG

  while (!should_exit)
  {
    enum Command cmd = get_next(input_no);

    // DBG
    if (cmd == CMD_INVALID)
    {
      printf("Thread %d found invalid command at line %d.\n", arg->index, arg->line + 1); // TODO: Remove
      cleanup(input_no);
    }
    else if (cmd == CMD_WAIT || cmd == CMD_BARRIER)
    {
      // must always be checked for execution
      should_exit = handle_command(cmd, arg, input_no);
    }
    else if (cmd == EOC)
    {
      break;
    }
    else
    {
      // only execute if on assigned lines
      // local reads to line are safe, only I write to line
      if (arg->line % used_threads == arg->index)
        should_exit = handle_command(cmd, arg, input_no);
      else
      {
        cleanup(input_no);
        break;
      }
    }
    for (unsigned int i = 0; i < used_threads; i++)
    {
      if (waited_threads[i] == 1)
      {
        ems_wait(waited_threads[i]);
      }
    }
    waited_threadsInitialized();
    pthread_mutex_lock(&arg->line_lock);
    arg->line++;
    pthread_mutex_unlock(&arg->line_lock);
  }

  close(input_no);
  printf("Thread %d finished and closed fd %d.\n", arg->index, input_no);
  thread_exit(arg, THREAD_FINISHED);
  //Should not execute
  return NULL;
}

int handle_command(enum Command cmd, struct thread_info *my_info, int input_no)
{
  unsigned int event_id, delay, thread_id;
  size_t num_rows, num_columns, num_coords;
  size_t xs[MAX_RESERVATION_SIZE], ys[MAX_RESERVATION_SIZE];

  printf("%d Handling command %d.\n", my_info->index, (int)cmd);

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
    printf("Handling SHOW.\n");
    ems_list_events(out_num, out_lock);
    break;

  case CMD_SHOW:
    if (parse_show(input_no, &event_id) != 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    printf("Handling SHOW.\n");
    if (ems_show(event_id, out_num, out_lock))
    {
      fprintf(stderr, "Failed to show event\n");
    }
    break;

  case CMD_WAIT:
    if (parse_wait(input_no, &delay, &thread_id) == -1)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }
    if (delay <= 0)
      break;
    if (thread_id == 0)
    {
      for (unsigned int i = 0; i < used_threads; i++)
      {
        if (i == my_info->index)
          continue;
        waited_threads[i] += delay;
      }
    }
    else if (thread_id - 1 == my_info->index)
    {
      waited_threads[my_info->index] += delay;
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
    thread_exit(my_info, THREAD_BARRIER);
    break;

  case CMD_EMPTY:
    break;

  case EOC:
    return 1;
  }

  return 0;
}

void waited_threadsInitialized()
{
  waited_threads = malloc(used_threads * sizeof(int));
  for (unsigned int i = 0; i < used_threads; i++)
  {
    waited_threads[i] = 0;
  }
}

void free_waited_threads()
{
  free(waited_threads);
}

void thread_exit(struct thread_info *me, int code)
{
  me->exit_code = code;
  pthread_exit(&me->exit_code);
}
