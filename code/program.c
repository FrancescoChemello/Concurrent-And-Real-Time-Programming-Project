/**
 * @author Chemello Francesco 2121346 
 *
 * Ex 01:
 * Acquisition of frames from a Webcam in Linux and storage of the frames on disk. Variation in
 * disk write shall not affect the frequency at which frames are acquired. Therefore the task
 * carrying out frame write on disk shall be decoupled from the task supervising frames
 * acquisition. Task implementation can be via Linux processes or Threads.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <linux/videodev2.h>    // for v4l2
#include <sys/ioctl.h>          // for input/output control
#include <semaphore.h>          // to define semaphores
#include <sys/mman.h>           // for memory map
#include <unistd.h>

#define DEVICE "/dev/video0"
#define BUFFER_SIZE 10
#define CHECK_IOCTL_STATUS(message)
#define SHARED_MEMORY_NAME "/shared_buffer"     // give a name to the shared memory -> identify the area

struct BufferData {
    int head;                   // head of the buffer
    int tail;                   // tail of the buffer
    int frame_size;             // size of the frame
    sem_t mutex;                // semaphore to protect the shared buffer
    sem_t data_avail;           // semaphore to signal that the buffer is empty
    sem_t room_avail;            // semaphore to signal that the buffer is full
    unsigned char buffer[];     // buffer to store the frames (in the end!)
};

char * usage_string = "Usage: %s <format> <height> <width> <framerate> <timeout>\n";
struct BufferData * sharedBuf;

static void frame_consumer(){
    unsigned char frame [sharedBuf->frame_size];
    while(1){
        sem_wait(&sharedBuf->data_avail);
        // enter critical section
        sem_wait(&sharedBuf->mutex);
        //collect the frame from the buffer
        memcpy(frame, sharedBuf->buffer[sharedBuf->head], sizeof(sharedBuf->frame_size));
        sharedBuf->head = (sharedBuf->head + 1) % BUFFER_SIZE;      // circular array
        sem_post(&sharedBuf->room_avail);
        // exit critical section
        sem_post(&sharedBuf->mutex);

        // TODO: save the frame in the disk ...

    }
}

static void frame_producer(){
    unsigned char frame [sharedBuf->frame_size];
    while(1){

        // TODO: get the frame from the videocamera ...

        sem_wait(&sharedBuf->room_avail);
        // enter critical section
        sem_wait(&sharedBuf->mutex);
        memcpy(sharedBuf->buffer[sharedBuf->tail], frame, sizeof(sharedBuf->frame_size));
        sharedBuf->tail = (sharedBuf->tail + 1) % BUFFER_SIZE;
        sem_post(&sharedBuf->data_avail);
        // exit critical section
        sem_post(&sharedBuf->mutex);
    }
}

int main(int argc, char **argv){
    int fd, status, memsh;

    int frame_size, height, width;
    
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_streamparm streamparm;

    if(argc == 1){ 
        printf(usage_string,argv[0]); 
        exit(EXIT_FAILURE);
    }

    printf("Format: %s - Height: %d - Width: %d - Frame Rate: %d - Timeout: %d\n", argv[1], atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));

    // 1. Check the device

    // Check if it can be opened
    fd = open(DEVICE, O_RDWR);
    if(fd == -1){
        perror("Error opening device");
        exit(EXIT_FAILURE);
    }

    // Check if it supports querying capabilities
    status = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    CHECK_IOCTL_STATUS("Error querying capability");
    if(!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("Streaming NOT supported\n");
        exit(EXIT_FAILURE);
    }

    // 2. Negotiate the format

    // Initialize the format structure
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // Setting the format
    if(!strcmp(argv[1],"MJPG") || !strcmp(argv[1],"mjpg")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    } else if (!strcmp(argv[1],"YUYV") || !strcmp(argv[1],"yuyv")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    } else {
        printf("Format %s not supported\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    // Setting the frame size
    fmt.fmt.pix.height = atoi(argv[2]);
    fmt.fmt.pix.width = atoi(argv[3]);
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    // Check if it supports the format
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("Error setting format");
        exit(EXIT_FAILURE);
    }

    // Check the format accepted
    printf("Accepted format: %c%c%c%c, %dx%d\n", 
        fmt.fmt.pix.pixelformat & 0xFF, 
        (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
        fmt.fmt.pix.width, fmt.fmt.pix.height);

    // Setting frame rate
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = atoi(argv[4]);
    
    // Check if it supports the frame rate
    if (ioctl(fd, VIDIOC_S_PARM, &streamparm) == -1) {
        perror("Error setting frame rate");
        exit(EXIT_FAILURE);
    }

    // Check the frame rate accepted
    printf("Frame rate accepted: %d/%d FPS\n",
        streamparm.parm.capture.timeperframe.denominator,
        streamparm.parm.capture.timeperframe.numerator);

    // Check the timeout value
    if (atoi(argv[5]) < 0) {
        printf("Timeout must be a positive value\n");
        exit(EXIT_FAILURE);
    }else{
        printf("Timeout: %d\n", atoi(argv[5]));
    }
    
    // 3. Shared buffer creation with MMap 

    // Dim
    height = fmt.fmt.pix.height;
    width = fmt.fmt.pix.width;

    if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV){
        frame_size = height * width * 2;    // 2 bytes for each pixel
    } else if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG){
        frame_size = height * width * 3;    // 3 bytes for each pixel
    }

    printf("Frame Size: %d\n", frame_size);

    // Definition of the buffer
    int total_size = sizeof(struct BufferData) + (BUFFER_SIZE * frame_size);

    memsh = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);

    // Check if I create the shared memory
    if(memsh == -1){
        perror("Shared memory creation failed");
        exit(EXIT_FAILURE);
    }

    // Check if I set the correct size of memory (= total_size)
    if (ftruncate(memsh, total_size) == -1) {
        perror("Errore ftruncate");
        exit(EXIT_FAILURE);
    }

    sharedBuf = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, memsh, 0);

    if(sharedBuf == MAP_FAILED){
        perror("Mmap failed");
        exit(EXIT_FAILURE);
    }

    sharedBuf->head = 0;
    sharedBuf->tail = 0;
    sharedBuf->frame_size = frame_size;
    sem_init(&sharedBuf->mutex, 1, 1);
    sem_init(&sharedBuf->data_avail, 1, 0);
    sem_init(&sharedBuf->room_avail, 1, BUFFER_SIZE);

    printf("Memoria condivisa creata con frame_size = %d bytes\n", frame_size);



    munmap(sharedBuf, total_size);
    shm_unlink(SHARED_MEMORY_NAME);
    
    return 0;
}