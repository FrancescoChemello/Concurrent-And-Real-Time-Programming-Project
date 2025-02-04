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
#include <unistd.h>
#include <sys/wait.h>
#include <linux/videodev2.h>    // for v4l2
#include <sys/ioctl.h>          // for input/output control
#include <semaphore.h>          // to define semaphores
#include <sys/mman.h>           // for memory map
#include <time.h>               // for the timer

#define DEVICE "/dev/video0"
#define BUFFER_SIZE 100
#define BUFFER_DIM 30
// give a name to the shared memory -> identify the area
#define SHARED_MEMORY_NAME "/shared_buffer"         // shared buffer
#define SHARED_FINISHED_CONDITION "/shared_finish"  //finish condition

#define CLIP(value) ((value) < 0 ? 0 : ((value) > 255 ? 255 : (value)))

struct BufferData {
    int head;                   // head of the buffer
    int tail;                   // tail of the buffer
    int height;                 // height of the frame
    int width;                  // width of the frame
    int frame_size;             // size of the frame
    char format[4];             // the type (MJPG or YUYV)
    sem_t mutex;                // semaphore to protect the shared buffer
    sem_t data_avail;           // semaphore to signal that the buffer is empty
    sem_t room_avail;           // semaphore to signal that the buffer is full
    unsigned char buffer[];     // buffer to store the frames (in the end!)
};

typedef struct {
    sem_t mutex;                // semaphore to protect the variable
    char finish;                // char to signal the termination (0 = false, 1 = true)
} finish_t;

void *buffer_ptrs[BUFFER_DIM];          // array of pointers to the buffers
size_t buffer_lengths[BUFFER_DIM];      // array of lengths of the buffers


char * usage_string = "Usage: %s <format> <height> <width> <framerate> <timeout>\n";
struct BufferData * sharedBuf;
finish_t * sharedFin;
int vd, timer;

/**
 * @brief Function that convert the YUYV format into RGB
 * @param yuyv pointer to the YUYV frame
 * @param rgb pointer to the RGB frame
 * @param width width of the frame
 * @param height height of the frame
 * @return
 */
void yuyv_to_rgb(unsigned char *yuyv, unsigned char *rgb, int width, int height) {
    int pixel_count = width * height;
    for (int i = 0, j = 0; i < pixel_count * 2; i += 4, j += 6) {
        int Y1 = yuyv[i];
        int U  = yuyv[i + 1];
        int Y2 = yuyv[i + 2];
        int V  = yuyv[i + 3];

        int C1 = Y1 - 16;
        int C2 = Y2 - 16;
        int D  = U - 128;
        int E  = V - 128;

        rgb[j] = CLIP((298 * C1 + 409 * E + 128) >> 8);                     // R1
        rgb[j + 1] = CLIP((298 * C1 - 100 * D - 208 * E + 128) >> 8);       // G1
        rgb[j + 2] = CLIP((298 * C1 + 516 * D + 128) >> 8);                 // B1

        rgb[j + 3] = CLIP((298 * C2 + 409 * E + 128) >> 8);                 // R2
        rgb[j + 4] = CLIP((298 * C2 - 100 * D - 208 * E + 128) >> 8);       // G2
        rgb[j + 5] = CLIP((298 * C2 + 516 * D + 128) >> 8);                 // B2
    }
}

/**
 * @brief Function that consumes the frames from the shared buffer and saves them on the disk
 * @param 
 * @return
 */
static void frame_consumer(){

    char type[4];
    char filename[20];
    int frame_numb = 0;

    while(1){

        sem_wait(&sharedBuf->data_avail);
        // enter critical section
        sem_wait(&sharedBuf->mutex);
        
        //termination condition
        sem_wait(&sharedFin->mutex);
        if(sharedFin->finish == 1 && (sharedBuf->head == sharedBuf->tail)){
            sem_post(&sharedFin->mutex);
            return;
        }
        sem_post(&sharedFin->mutex);

        unsigned char frame [sharedBuf->frame_size];
        strcpy(type, sharedBuf->format);
        int h = sharedBuf->height;
        int w = sharedBuf->width;
        //collect the frame from the buffer
        memcpy(frame, &sharedBuf->buffer[sharedBuf->head * sharedBuf->frame_size], sharedBuf->frame_size);
        sharedBuf->head = (sharedBuf->head + 1) % BUFFER_SIZE;      // circular array
        sem_post(&sharedBuf->room_avail);
        // exit critical section
        sem_post(&sharedBuf->mutex);

        // save the frame in the disk (folder frame)
        if(strncmp(type, "YUYV", 4) == 0){
            sprintf(filename, "frame/converted_frame_%d.jpg", frame_numb);        // save the frame in .jpg
            // convert the YUYV into RGB
            unsigned char rgb_frame[ w * h * 3];
            yuyv_to_rgb(frame, rgb_frame, w, h);
            FILE * frame_file = fopen(filename, "wb");
            if(frame_file){
                fwrite(rgb_frame, 1, sizeof(rgb_frame), frame_file);
                fclose(frame_file);
            }else{
                perror("Error saving the frame\n");
            }
        }else{
            sprintf(filename, "frame/frame_%d.jpg", frame_numb);        // save the frame in .jpg
            FILE * frame_file = fopen(filename, "wb");
            if(frame_file){
                fwrite(frame, 1, sizeof(frame), frame_file);
                fclose(frame_file);
            }else{
                perror("Error saving the frame\n");
            }
        }
        frame_numb++;

    }
}

/**
 * @brief Function that produces the frames from the webcam and stores them in the shared buffer (mmap)
 * @param
 * @return
 */
static void frame_producer(){   

    int status;
    struct v4l2_buffer buf;    // buffer structure

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // start the streaming
    if(ioctl(vd, VIDIOC_STREAMON, &type) == -1){
        perror("Errore avvio streaming");
        exit(EXIT_FAILURE);
    }

    time_t start = time(0);    // get the current time

    // loop until the timer expires
    while(difftime(time(0), start) < timer){

        // extract the frame from the buffer
        if(ioctl(vd, VIDIOC_DQBUF, &buf) == -1){
            perror("Dequeue buffer error");
            continue;
        }

        // check if there is room to store the frame
        sem_wait(&sharedBuf->mutex);
        if(sharedBuf->head == (sharedBuf->tail + 1) % BUFFER_SIZE){
            // no room available
            perror("Buffer is full\n");
            sem_post(&sharedBuf->mutex);
            continue;   // restart
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

        // re-enqueue the buffer
        if(ioctl(vd, VIDIOC_QBUF, &buf) == -1){
            perror("Queue buffer error");
            continue;
        }
    }

    // stop streaming
    if(ioctl(vd, VIDIOC_STREAMOFF, &type) == -1){
        perror("Errore stop streaming");
        exit(EXIT_FAILURE);
    }

    sem_wait(&sharedFin->mutex);
    sharedFin->finish = 1;      // notify the consumer that the producer has finished
    sem_post(&sharedFin->mutex);

    sem_wait(&sharedBuf->mutex);
    sem_post(&sharedBuf->data_avail);
    sem_post(&sharedBuf->mutex);

}

/**
 * @brief Main function
 * @param argc number of arguments
 * @param argv array of arguments
 */
int main(int argc, char **argv){
    int status, memsh, fin;
    int frame_size, height, width;
    
    struct v4l2_capability cap;             // capability structure
    struct v4l2_format fmt;                 // format structure
    struct v4l2_streamparm streamparm;      // stream parameter structure
    struct v4l2_requestbuffers req;         // request buffer structure
    struct v4l2_buffer buf;                 // buffer structure

    if(argc == 1){ 
        printf(usage_string, argv[0]); 
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
        timer = atoi(argv[5]);
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
    sharedBuf->height = height;
    sharedBuf->width = width;
    sharedBuf->frame_size = frame_size;
    if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG){
        strcpy(sharedBuf->format, "MJPG");
    }else if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV){
        strcpy(sharedBuf->format, "YUYV");
    }
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

    // memory map for ending condition
    fin = shm_open(SHARED_FINISHED_CONDITION, O_CREAT | O_RDWR, 0666);

    if(fin == -1){
        perror("Shared memory creation failed");
        exit(EXIT_FAILURE);
    }

    // check if I set the correct size of memory (= total_size)
    if(ftruncate(fin, sizeof(finish_t)) == -1){
        perror("Errore ftruncate");
        exit(EXIT_FAILURE);
    }

    sharedFin = mmap(NULL, sizeof(finish_t), PROT_READ | PROT_WRITE, MAP_SHARED, fin, 0);

    // check if I create the shared memory
    if(sharedFin == MAP_FAILED){
        perror("Mmap failed");
        exit(EXIT_FAILURE);
    }

    sharedFin->finish = 0;
    sem_init(&sharedFin->mutex, 1, 1);

    // start the producer and consumer
    pid_t pid;
    pid = fork();

    if(pid < 0){
        perror("Error fork");
        exit(EXIT_FAILURE);
    }
    if(pid == 0){
        frame_consumer();
        exit(0);
    }
    
    frame_producer();

    // wait the consumer
    waitpid(pid, NULL, 0);
    printf("End acquisition\n");

    // free the memory
    munmap(sharedBuf, total_size);
    shm_unlink(SHARED_MEMORY_NAME);
    close(memsh);
    munmap(sharedFin, sizeof(finish_t));
    shm_unlink(SHARED_FINISHED_CONDITION);
    close(fin);

    // close the streaming
    for(int i = 0; i < BUFFER_DIM; i++){
        munmap(buffer_ptrs[i], buffer_lengths[i]);
    }
    printf("Free memory\n");

    // close the device (videocamera)
    close(vd);
    printf("Close the videocamera\n");
    
    return 0;
}