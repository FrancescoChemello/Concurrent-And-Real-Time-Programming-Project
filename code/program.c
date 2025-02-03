/**
 * @author Chemello Francesco 2121346
 * @version 1.0
 *
 * @paragraph Exercise 1: 
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
#include <time.h>               // for the timer
#include <unistd.h>
#include <sys/wait.h>

#define DEVICE "/dev/video0"
#define BUFFER_SIZE 10
#define BUFFER_DIM 10
#define SHARED_MEMORY_NAME "/shared_buffer"     // give a name to the shared memory -> identify the area

struct BufferData {
    int head;                   // head of the buffer
    int tail;                   // tail of the buffer
    int frame_size;             // size of the frame
    sem_t mutex;                // semaphore to protect the shared buffer
    sem_t data_avail;           // semaphore to signal that the buffer is empty
    sem_t room_avail;           // semaphore to signal that the buffer is full
    unsigned char buffer[];     // buffer to store the frames (in the end!)
};

void *buffer_ptrs[BUFFER_DIM];          // array of pointers to the buffers
size_t buffer_lengths[BUFFER_DIM];      // array of lengths of the buffers


char * usage_string = "Usage: %s <format> <height> <width> <framerate> <timeout>\n";
struct BufferData * sharedBuf;

/**
 * @brief Function that consumes the frames from the shared buffer and saves them on the disk (raw)
 * @param frame_size size of the frame
 * @return void
 */
static void frame_consumer(int frame_size){

    unsigned char frame [frame_size];
    char filename[20];
    int frame_numb = 0;

    while(1){

        sem_wait(&sharedBuf->data_avail);
        // enter critical section
        sem_wait(&sharedBuf->mutex);
        //collect the frame from the buffer
        memcpy(frame, &sharedBuf->buffer[sharedBuf->head * sharedBuf->frame_size], sharedBuf->frame_size);
        sharedBuf->head = (sharedBuf->head + 1) % BUFFER_SIZE;      // circular array
        sem_post(&sharedBuf->room_avail);
        // exit critical section
        sem_post(&sharedBuf->mutex);

        // save the frame in the disk (folder frame)
        sprintf(filename, "frame/frame_%d.raw", frame_numb);
        FILE * frame_file = fopen(filename, "wb");
        if(frame_file){
            fwrite(frame, 1, frame_size, frame_file);
            fclose(frame_file);
        }else{
            printf("Error saving the frame\n");
        }
        frame_numb++;

    }
}

/**
 * @brief Function that produces the frames from the webcam and stores them in the shared buffer (mmap)
 * @param vd pointer to the video device
 * @param timer pointer to the timer
 * @return void
 */
static void frame_producer(int * vd, int * timer){   

    int status;
    struct v4l2_buffer buf;    // buffer structure

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // start the streaming
    if(ioctl(*vd, VIDIOC_STREAMON, &type) == -1){
        perror("Errore avvio streaming");
        exit(EXIT_FAILURE);
    }

    time_t start = time(0);    // get the current time

    // loop until the timer expires
    while(difftime(time(0), start) < *timer){

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if(ioctl(*vd, VIDIOC_DQBUF, &buf) == -1){
            perror("Dequeue buffer error");
            exit(EXIT_FAILURE);
        }

        // check if there is room to store the frame
        sem_wait(&sharedBuf->mutex);
        if(sharedBuf->head == (sharedBuf->tail + 1) % BUFFER_SIZE){
            // no room available
            printf("Buffer is full\n");
            sem_post(&sharedBuf->mutex);
            exit(EXIT_FAILURE);
        }
        sem_post(&sharedBuf->mutex);

        sem_wait(&sharedBuf->room_avail);
        // enter critical section
        sem_wait(&sharedBuf->mutex);
        unsigned char * buf_frame = &sharedBuf->buffer[sharedBuf->tail * sharedBuf->frame_size];
        memcpy(buf_frame, buffer_ptrs[buf.index], sharedBuf->frame_size);
        sharedBuf->tail = (sharedBuf->tail + 1) % BUFFER_SIZE;
        sem_post(&sharedBuf->data_avail);
        // exit critical section
        sem_post(&sharedBuf->mutex);

        if(ioctl(*vd, VIDIOC_QBUF, &buf) == -1){
            perror("Queue buffer error");
            exit(EXIT_FAILURE);
        }
    }

    // stop streaming
    if(ioctl(*vd, VIDIOC_STREAMOFF, &type) == -1){
        perror("Errore stop streaming");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Main function
 * @param argc number of arguments
 * @param argv array of arguments
 */
int main(int argc, char **argv){
    int vd, status, memsh;
    int frame_size, height, width;
    int timeout;
    
    struct v4l2_capability cap;             // capability structure
    struct v4l2_format fmt;                 // format structure
    struct v4l2_streamparm streamparm;      // stream parameter structure
    struct v4l2_requestbuffers req;         // request buffer structure
    struct v4l2_buffer buf;                 // buffer structure

    if(argc == 1){ 
        printf(usage_string,argv[0]); 
        exit(EXIT_FAILURE);
    }

    printf("Format: %s - Height: %d - Width: %d - Frame Rate: %d - Timeout: %d\n", argv[1], atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));

    // check if it can be opened
    vd = open(DEVICE, O_RDWR);
    if(vd == -1){
        perror("Error opening device");
        exit(EXIT_FAILURE);
    }

    // check if it supports querying capabilities
    if(ioctl(vd, VIDIOC_QUERYCAP, &cap) == -1){
        perror("Error querying capability");
        exit(EXIT_FAILURE);
    }

    // check if it supports streaming
    if(!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("Streaming NOT supported\n");
        exit(EXIT_FAILURE);
    }

    // videocamera setting negotiation 

    // initialize the format structure
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // setting the format
    if(!strcmp(argv[1],"MJPG") || !strcmp(argv[1],"mjpg")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    } else if (!strcmp(argv[1],"YUYV") || !strcmp(argv[1],"yuyv")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    } else {
        printf("Format %s not supported\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    // setting the frame size
    fmt.fmt.pix.height = atoi(argv[2]);
    fmt.fmt.pix.width = atoi(argv[3]);
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    // check if it supports the format
    if(ioctl(vd, VIDIOC_S_FMT, &fmt) == -1){
        perror("Error setting format");
        exit(EXIT_FAILURE);
    }

    // check the format accepted
    printf("Accepted format: %c%c%c%c, %dx%d\n", 
        fmt.fmt.pix.pixelformat & 0xFF, 
        (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
        fmt.fmt.pix.width, fmt.fmt.pix.height);

    // setting frame rate
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = atoi(argv[4]);
    
    // check if it supports the frame rate
    if(ioctl(vd, VIDIOC_S_PARM, &streamparm) == -1){
        perror("Error setting frame rate");
        exit(EXIT_FAILURE);
    }

    // check the frame rate accepted
    printf("Frame rate accepted: %d/%d FPS\n",
        streamparm.parm.capture.timeperframe.denominator,
        streamparm.parm.capture.timeperframe.numerator);

    // check the timeout value
    if(atoi(argv[5]) < 0){
        printf("Timeout must be a positive value\n");
        exit(EXIT_FAILURE);
    }else{
        printf("Timeout: %d\n", atoi(argv[5]));
        timeout = atoi(argv[5]);
    }

    // dim
    height = fmt.fmt.pix.height;
    width = fmt.fmt.pix.width;

    if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV){
        frame_size = height * width * 2;    // 2 bytes for each pixel
    }else if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG){
        frame_size = height * width * 3;    // 3 bytes for each pixel
    }

    printf("Frame Size: %d\n", frame_size);

    // definition of the buffer
    int total_size = sizeof(struct BufferData) + (BUFFER_SIZE * frame_size);

    memsh = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);

    // check if I create the shared memory
    if(memsh == -1){
        perror("Shared memory creation failed");
        exit(EXIT_FAILURE);
    }

    // check if I set the correct size of memory (= total_size)
    if(ftruncate(memsh, total_size) == -1){
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

    // set the structure for the request
    memset(&req, 0, sizeof(req));               // set to 0 the structure
    req.count = BUFFER_DIM;                     // set the number of buffers
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;     // set the type of buffer (video capture)
    req.memory = V4L2_MEMORY_MMAP;              // set the memory type (mmap, direct access in memory)

    // check if the request is accepted
    if(ioctl(vd, VIDIOC_REQBUFS, &req) == -1){
        perror("VIDIOC_REQBUFS error");
        exit(EXIT_FAILURE);
    }

    // configuration of the buffers
    for(int i = 0; i < BUFFER_DIM; i++){

        memset(&buf, 0, sizeof(buf));               // set to 0 the structure
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;     // set the type of buffer (video capture)
        buf.memory = V4L2_MEMORY_MMAP;              // set the memory type (mmap, direct access in memory)
        buf.index = i;                              // set the index of the buffer

        // check if the buffer is accepted
        if(ioctl(vd, VIDIOC_QUERYBUF, &buf) == -1){
            perror("VIDIOC_QUERYBUF error");
            exit(EXIT_FAILURE);
        }

        // map the buffer in memory (mmap)
        buffer_ptrs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, vd, buf.m.offset);
        if(buffer_ptrs[i] == MAP_FAILED){
            perror("MMAP failure");
            exit(EXIT_FAILURE);
        }

        buffer_lengths[i] = buf.length;             // set the length of the buffer

        // enqueue the buffer
        if(ioctl(vd, VIDIOC_QBUF, &buf) == -1){
            perror("VIDIOC_QBUF error");
            exit(EXIT_FAILURE);
        }
    }

    printf("Configuration of the buffers completed\n");

    // start the producer and consumer
    pid_t pid = fork();

    if(pid < 0){
        perror("Error fork");
        exit(EXIT_FAILURE);
    }
    if(pid == 0){
        frame_consumer(frame_size);
        exit(0);
    }else{
        frame_producer(&vd, &timeout);
        exit(0);
    }

    // wait the consumer
    waitpid(pid, NULL, 0);

    printf("End acquisition\n");

    // free the memory
    munmap(sharedBuf, total_size);
    shm_unlink(SHARED_MEMORY_NAME);
    
    return 0;
}