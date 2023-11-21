#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

#define BUFFER_SIZE 1024


void readAllFilesOfDirectory(char *path);
int ReadAll(char *file);
int main(int argc, char *argv[])
{

  char *path = "/home/poico/SO/Praticas/Proj1/teste";
  readAllFilesOfDirectory(path);
  return 0;   
}

void readAllFilesOfDirectory(char *path){
  DIR *dir = opendir(path);
  if (dir == NULL) {
    printf("Erro");
    //fprintf(stderr, "Error opening directory%s\n", perror(errno));        
  }

  struct dirent *entry;
  
  while ((entry = readdir(dir)) != NULL) {
    printf("Found file or directory: %s\n", entry->d_name);
    ReadAll(entry->d_name);
  }

  closedir(dir);
}

int ReadAll(char *file){
  int fd = open(file, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "open error: %s\n", strerror(errno));
    return -1;
  }

  char buffer[BUFFER_SIZE];
  ssize_t bytes_read;
  while ((bytes_read = read(fd, buffer, sizeof(buffer)-1)) > 0) {
    buffer[bytes_read] = '\0';  // Null-terminate the string
    printf("%s", buffer);
  }

  if (bytes_read < 0) {
    fprintf(stderr, "read error: %s\n", strerror(errno));
    return -1;
  }

  close(fd);
  return 0;
}