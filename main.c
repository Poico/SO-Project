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
  off_t pos;
} *thread_infos;

unsigned int used_threads = 0;

void process_args(int argc, char *argv[], DIR **dir);
unsigned int launch_processes(DIR *dir);
int child_main(struct dirent *dirent);
int process_file(struct dirent *dirent);
void handle_file(char *relativePath);
void handle_command(enum Command cmd, struct thread_info *my_info, int input_no);
void thread_exit(struct thread_info *me, int code, int fd);
int out_num;
pthread_mutex_t out_lock;
unsigned int *waited_threads;

void *thread_main(void *argument);

unsigned int state_access_delay_ms = STATE_ACCESS_DELAY_MS;
unsigned int max_proc = MAX_PROC, max_thread = MAX_THREADS;
char *glob_dirPath;

int main(int argc, char *argv[])
{
  DIR *dir;
  process_args(argc, argv, &dir);

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
  closedir(dir);
  return 0;
}

void process_args(int argc, char *argv[], DIR **dir)
{
  if (argc > 4)
  {
    char *endptr;
    unsigned long int delay = strtoul(argv[4], &endptr, 10);

    if (*endptr != '\0' || delay > UINT_MAX)
    {
      fprintf(stderr, "Invalid delay value or value too large\n");
      exit(EXIT_FAILURE);
    }

    state_access_delay_ms = (unsigned int)delay;
  }
  if (argc > 3)
  {
    char *endptr;
    unsigned long int max_threads = strtoul(argv[3], &endptr, 10);

    if (*endptr != '\0' || max_threads > UINT_MAX)
    {
      fprintf(stderr, "Invalid maximum threads count value or value too large\n");
      exit(EXIT_FAILURE);
    }

    max_thread = (unsigned int)max_threads;
  }
  if (argc > 2)
  {
    char *endptr;
    unsigned long int proc_count = strtoul(argv[2], &endptr, 10);

    if (*endptr != '\0' || proc_count > UINT_MAX)
    {
      fprintf(stderr, "Invalid maximum process count value or value too large\n");
      exit(EXIT_FAILURE);
    }

    max_proc = (unsigned int)proc_count;
  }
  if (argc > 1)
  {
    *dir = opendir(argv[1]);
  }
  else
  {
    fprintf(stderr, "You must always provide a path to a job directory.\n");
  }
}

unsigned int launch_processes(DIR *dir)
{
  unsigned int processCount = 0;
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
    return SUCESS;
  }

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
  int fd = open(relativePath, O_RDONLY);
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

  thread_infos = malloc(used_threads * sizeof(struct thread_info));

  // duplicate input_no with dup
  for (unsigned int i = 0; i < used_threads; i++)
  {
    thread_infos[i].index = i;
    strcpy(thread_infos[i].path, relativePath);
    thread_infos[i].line = 0;
    thread_infos[i].pos = 0;
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
      pthread_join(thread_infos[i].id, (void **)&ret_code);
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
  int input_no = open(arg->path, O_RDONLY);
  lseek(input_no, arg->pos, SEEK_SET);

  while (1)
  {
    enum Command cmd = get_next(input_no);

    if (cmd == CMD_WAIT || cmd == CMD_BARRIER)
    {
      // must always be checked for execution
      handle_command(cmd, arg, input_no);
    }
    else if (cmd == EOC)
    {
      break;
    }
    else //general commands
    {
      // only execute if on assigned lines
      // local reads to line are safe, only I write to line
      if (arg->line % used_threads == arg->index){
        handle_command(cmd, arg, input_no);
      }
      else if (cmd == CMD_CREATE || cmd == CMD_RESERVE || cmd == CMD_SHOW){
        cleanup(input_no);
      }
    }

    pthread_mutex_lock(&arg->line_lock);
    arg->line++;
    pthread_mutex_unlock(&arg->line_lock);
  }

  close(input_no);
  thread_exit(arg, THREAD_FINISHED, input_no);
  // Should not execute
  return NULL;
}

void handle_command(enum Command cmd, struct thread_info *my_info, int input_no)
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
    ems_list_events(out_num, &out_lock);
    break;

  case CMD_SHOW:
    if (parse_show(input_no, &event_id) != 0)
    {
      fprintf(stderr, "Invalid command. See HELP for usage\n");
      break;
    }

    if (ems_show(event_id, out_num, &out_lock))
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
      ems_wait(delay);
    }
    else if (thread_id - 1 == my_info->index)
    {
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
    pthread_mutex_lock(&my_info->line_lock);
    my_info->line++;
    pthread_mutex_unlock(&my_info->line_lock);
    thread_exit(my_info, THREAD_BARRIER, input_no);
    break;

  case CMD_EMPTY:
    break; // Handled externally

  case EOC:
    break; // Handled externally
  }
}

void thread_exit(struct thread_info *me, int code, int fd)
{
  me->exit_code = code;
  me->pos = lseek(fd, 0, SEEK_CUR);
  pthread_exit(&me->exit_code);
}
