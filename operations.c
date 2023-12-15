#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "eventlist.h"
#include "constants.h"

static struct EventList *event_list = NULL;
static unsigned int state_access_delay_ms = 0;
int write_to_file(int fileDescriptor, char *buffer);
/// Calculates a timespec from a delay in milliseconds.
/// @param delay_ms Delay in milliseconds.
/// @return Timespec with the given delay.
static struct timespec delay_to_timespec(unsigned int delay_ms)
{
  return (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
}

/// Gets the event with the given ID from the state.
/// @note Will wait to simulate a real system accessing a costly memory resource.
/// @param event_id The ID of the event to get.
/// @return Pointer to the event if found, NULL otherwise.
static struct Event *get_event_with_delay(unsigned int event_id)
{
  struct timespec delay = delay_to_timespec(state_access_delay_ms);
  nanosleep(&delay, NULL); // Should not be removed

  return get_event(event_list, event_id);
}

/// Gets the seat with the given index from the state.
/// @note Will wait to simulate a real system accessing a costly memory resource.
/// @param event Event to get the seat from.
/// @param index Index of the seat to get.
/// @return Pointer to the seat.
static unsigned int *get_seat_with_delay(struct Event *event, size_t index)
{
  struct timespec delay = delay_to_timespec(state_access_delay_ms);
  nanosleep(&delay, NULL); // Should not be removed

  return &event->data[index];
}

/// Gets the index of a seat.
/// @note This function assumes that the seat exists.
/// @param event Event to get the seat index from.
/// @param row Row of the seat.
/// @param col Column of the seat.
/// @return Index of the seat.
static size_t seat_index(struct Event *event, size_t row, size_t col) { return (row - 1) * event->cols + col - 1; }

int ems_init(unsigned int delay_ms)
{
  if (event_list != NULL)
  {
    fprintf(stderr, "EMS state has already been initialized\n");
    return 1;
  }

  event_list = create_list();
  state_access_delay_ms = delay_ms;

  return event_list == NULL;
}

int ems_terminate()
{
  if (event_list == NULL)
  {
    fprintf(stderr, "EMS state must be initialized\n");
    return 1;
  }

  free_list(event_list);
  return 0;
}

int ems_create(unsigned int event_id, size_t num_rows, size_t num_cols)
{
  if (event_list == NULL)
  {
    fprintf(stderr, "EMS state must be initialized\n");
    return 1;
  }

  if (get_event_with_delay(event_id) != NULL)
  {
    fprintf(stderr, "Event already exists\n");
    return 1;
  }

  struct Event *event = malloc(sizeof(struct Event));

  if (event == NULL)
  {
    fprintf(stderr, "Error allocating memory for event\n");
    return 1;
  }

  event->id = event_id;
  event->rows = num_rows;
  event->cols = num_cols;
  event->reservations = 0;
  pthread_mutex_init(&event->lock, NULL);
  event->data = malloc(num_rows * num_cols * sizeof(unsigned int));

  if (event->data == NULL)
  {
    fprintf(stderr, "Error allocating memory for event data\n");
    free(event);
    return 1;
  }

  for (size_t i = 0; i < num_rows * num_cols; i++)
  {
    event->data[i] = 0;
  }

  if (append_to_list(event_list, event) != 0)
  {
    fprintf(stderr, "Error appending event to list\n");
    free(event->data);
    free(event);
    return 1;
  }

  return 0;
}

int ems_reserve(unsigned int event_id, size_t num_seats, size_t *xs, size_t *ys)
{
  if (event_list == NULL)
  {
    fprintf(stderr, "EMS state must be initialized\n");
    return 1;
  }

  struct Event *event = get_event_with_delay(event_id);

  if (event == NULL)
  {
    fprintf(stderr, "Event not found\n");
    return 1;
  }
  pthread_mutex_lock(&event->lock);
  unsigned int reservation_id = ++event->reservations;

  size_t i = 0;
  for (; i < num_seats; i++)
  {
    size_t row = xs[i];
    size_t col = ys[i];

    if (row <= 0 || row > event->rows || col <= 0 || col > event->cols)
    {
      fprintf(stderr, "Invalid seat\n");
      break;
    }

    if (*get_seat_with_delay(event, seat_index(event, row, col)) != 0)
    {
      fprintf(stderr, "Seat already reserved\n");
      break;
    }

    *get_seat_with_delay(event, seat_index(event, row, col)) = reservation_id;
  }
  
  // If the reservation was not successful, free the seats that were reserved.
  if (i < num_seats)
  {
    event->reservations--;
    for (size_t j = 0; j < i; j++)
    {
      *get_seat_with_delay(event, seat_index(event, xs[j], ys[j])) = 0;
    }
    pthread_mutex_unlock(&event->lock);
    return 1;
  }
  pthread_mutex_unlock(&event->lock);
  return 0;
}

int ems_show(unsigned int event_id, int file_no, pthread_mutex_t *out_lock)
{
  if (event_list == NULL)
  {
    fprintf(stderr, "EMS state must be initialized\n");
    return 1;
  }

  struct Event *event = get_event_with_delay(event_id);

  if (event == NULL)
  {
    fprintf(stderr, "Event not found\n");
    return 1;
  }

  pthread_mutex_lock(out_lock);
  for (size_t i = 1; i <= event->rows; i++)
  {
    char buffer[1024];
    for (size_t j = 1; j <= event->cols; j++)
    {
      unsigned int *seat = get_seat_with_delay(event, seat_index(event, i, j));
      sprintf(buffer, "%u", *seat);
      write_to_file(file_no, buffer);

      if (j < event->cols)
      {
        sprintf(buffer, " ");
        write_to_file(file_no, buffer);
      }
    }
    sprintf(buffer, "\n");
    write_to_file(file_no, buffer);
  }
  pthread_mutex_unlock(out_lock);

  return 0;
}

int ems_list_events(int file_no, pthread_mutex_t *out_lock)
{
  if (event_list == NULL)
  {
    fprintf(stderr, "EMS state must be initialized\n");
    return 1;
  }

  if (event_list->head == NULL)
  {
    write_to_file(file_no, "No events\n");    
    return 0;
  }


  pthread_mutex_lock(out_lock);
  struct ListNode *current = event_list->head;
  while (current != NULL)
  {
    char buffer[1024];
    sprintf(buffer, "Event: %u\n", (current->event)->id);
    write_to_file(file_no, buffer); //TODO: end this part
    current = current->next;
  }
  pthread_mutex_unlock(out_lock);

  return 0;
}

int write_to_file(int fileDescriptor, char *buffer)
{
  ssize_t bytesWritten;

  bytesWritten = write(fileDescriptor, buffer, strlen(buffer));

  if (bytesWritten == -1)
  {
    perror("Error writing to the file");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

void ems_wait(unsigned int delay_ms)
{
  struct timespec delay = delay_to_timespec(delay_ms);
  
  nanosleep(&delay, NULL);
}
