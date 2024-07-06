#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <errno.h>
#include <unistd.h>
#include <limits.h>

#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>

#ifdef _WIN32
  #define CLEAR "cls" 
#elif __linux__
  #define CLEAR "clear"
#endif

typedef struct {
  char **listHead;
  size_t size;
  size_t capacity;
}directoryList;

void freeList(directoryList *list){
  for(size_t i = 0;i < list->size;i++){
    free(list->listHead[i]);
  }
  free(list->listHead);
}

void storeDirectory(const char *path,directoryList *list) {
  if(list->size == list->capacity) {
    list->capacity *= 2;
    char **new_path = realloc(list->listHead,list->capacity * sizeof(char *));
    if(new_path == NULL) {
      freeList(list);
      fprintf(stderr,"Realloc: %s",strerror(errno));
      exit(EXIT_FAILURE);
    }
    list->listHead = new_path;
  }
  list->listHead[list->size] = strdup(path);
  if(list->listHead[list->size] == NULL){
    freeList(list);
    fprintf(stderr,"Strdup: %s",strerror(errno));
    exit(EXIT_FAILURE);
  }
  list->size++;
}

void traverseDirectory(const char *path,directoryList *list) {
  DIR *dir = opendir(path);
  if(dir == NULL) {
    freeList(list);
    fprintf(stderr,"Opendir: %s",strerror(errno));
    exit(EXIT_FAILURE);
  }
  struct dirent *entry;
  char fullpath[4096];

  while((entry = readdir(dir)) != NULL) {
    if(strcmp(entry->d_name,".") == 0 || strcmp(entry->d_name,"..") == 0) {
      continue;
    }

    if(entry->d_type == DT_DIR) {
      snprintf(fullpath,sizeof(fullpath),"%s/%s",path,entry->d_name);
      storeDirectory(fullpath,list);
      traverseDirectory(fullpath,list);
    }
  }
  closedir(dir);
}

/* Read all available inotify events from the file descriptor 'inotify_fd'.
   inotify_wd is the table of watch descriptors for the directory in argv.*/

void handle_events(int inotify_fd,const char *makePath) {
  /* Some systems cannot read integer variables if they are not
     properly aligned. On other systems, incorrect alignment may
     decrease performance. Hence, the buffer used for reading from
     the inotify file descriptor should have the same alignment as
     struct inotify_event. */

  char buffer[10 * sizeof(struct inotify_event) + NAME_MAX + 1]
    __attribute__ ((aligned(__alignof__(struct inotify_event))));

  const struct inotify_event *event;
  ssize_t bytes_read;

  while(true) {
    
    /* Read events. */
    bytes_read = read(inotify_fd,buffer,sizeof(buffer));

    /* If the nonblocking read() found no events to read, then
       it returns -1 with errno set to EAGAIN. In that case,
       we exit the loop. */

    if(0 >= bytes_read) {
      if(errno != EAGAIN) {
        fprintf(stderr,":Failed to read from file descriptor: '%s'",
                strerror(errno));
        close(inotify_fd);
        exit(EXIT_FAILURE);
      }
      break;
    }

    /* Loop over all events in the buffer. */

    for(char *ptr = buffer;ptr < buffer + bytes_read;
        ptr += sizeof(struct inotify_event) + event->len) {
      event = (const struct inotify_event *) ptr;

      /* Print event type. */

      char makeCommand[256];
      char killCommand[256];
      snprintf(makeCommand,sizeof(makeCommand),"cd %s && make kill",makePath);
      snprintf(killCommand,sizeof(killCommand),"cd %s && make",makePath);

      if(event->mask){
        printf("%s\n",makeCommand);
        printf("%s\n",killCommand);
      }
    }
  }
}

int main(int argc,char *argv[]) {
  /* Check if correct number of arguments are provided */
  if(argc > 3) {
    fprintf(stderr,":Usage: %s [PATH ...] [MAKEPATH ...]",
            argv[0]);
    exit(EXIT_FAILURE);
  }

  /* Check if arguments are correct. */

  if(access(argv[1],F_OK) != 0) {
    fprintf(stderr,":%s : %s",argv[1],strerror(errno));
    exit(EXIT_FAILURE);
  } else if(access(argv[2],F_OK) != 0) {
    fprintf(stderr,":%s : %s",argv[2],strerror(errno));
    exit(EXIT_FAILURE);
  }

  directoryList list = {
    .size = 0,
    .capacity = 10,
    .listHead =  NULL,
  };
  list.listHead = malloc(list.capacity * sizeof(char *));

  if(list.listHead == NULL) {
    fprintf(stderr,"malloc: %s",strerror(errno));
    exit(EXIT_FAILURE);
  }
  storeDirectory(argv[1],&list);
  traverseDirectory(argv[1],&list);

  /* Create the file descriptor for accessing the inotify API. */

  int inotify_fd = inotify_init1(IN_NONBLOCK);
  if(0 > inotify_fd) {
    fprintf(stderr,":Failed to initialize: '%s'",
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* Create the watch descriptor for watching paths. */

  int *inotify_wd = calloc(list.size,sizeof(int));
  if(inotify_wd == NULL) {
    fprintf(stderr,"Calloc: %s",strerror(errno));
    freeList(&list);
    exit(EXIT_FAILURE);
  }

  for(size_t i = 0;i < list.size;i++){
    inotify_wd[i] = inotify_add_watch(inotify_fd,list.listHead[i],
        IN_MODIFY);
    if(0 > inotify_wd[i]){
      fprintf(stderr,":Failed to add watch on '%s': '%s'",
              strerror(errno));
      free(inotify_wd);
      freeList(&list);
      close(inotify_fd);
      exit(EXIT_FAILURE);
    }
    printf("%s: FILE NO:%d : %s\n",list.listHead[i],
            inotify_wd[i],strerror(errno));
  }

  /* Declare structure for polling */

  char buffer[256];
  int bytes_read;
  nfds_t nfds = 2;
  struct pollfd poll_fd[2] = {
    {
      .fd = STDIN_FILENO,   /* Console Input */
      .events = POLLIN,     /* Inotify Input */
    },{
      .fd = inotify_fd,
      .events = POLLIN,
    }
  };

  while(true) {
    fflush(stdout);
    int poll_num = poll(poll_fd,nfds,-1);
    if(poll_num == -1) {
      if(errno == EINTR)
        continue;
      fprintf(stderr,":Failed to Poll: '%s'",
              strerror(errno));
      close(inotify_fd);
      exit(EXIT_FAILURE);
    }

    if(poll_num > 0) {
      if(poll_fd[0].revents & POLLIN) {
        if(0 < (bytes_read = read(STDIN_FILENO,&buffer,sizeof(buffer) - 1))) {
          buffer[bytes_read - 1] = '\0';
          if(strcmp(buffer,"exit") == 0) {
            close(inotify_fd);
            exit(EXIT_SUCCESS);
          } else if(strcmp(buffer,"rs") == 0) {
            printf("RESTARTING\n");
            continue;
          }
        } else if(0 > bytes_read) {

        }
      }
      if(poll_fd[1].revents & POLLIN) {
        handle_events(inotify_fd,argv[2]);
      }
    }
  }

  printf(":Listening for events stopped.\n");

  free(&list);
  free(inotify_wd);

  close(inotify_fd);
  exit(EXIT_SUCCESS);
}
