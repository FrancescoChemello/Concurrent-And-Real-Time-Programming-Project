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
#include <unistd.h>
#include <time.h>               // for the timer
#include <pthread.h>            // for pthreads
#include <linux/videodev2.h>    // for v4l2
#include <sys/ioctl.h>          // for input/output control
#include <fcntl.h>              // for file control options
#include <sys/mman.h>           // for memory map

#define DEVICE "/dev/video0"
#define BUFFER_SIZE 100
#define BUFFER_DIM 50

#define CLIP(value) ((value) < 0 ? 0 : ((value) > 255 ? 255 : (value)))

pthread_mutex_t buf_mutex;                      // semaphore for the shared buffer
pthread_mutex_t fin_mutex;                      // semaphore for the finish condition
pthread_mutex_t frame_numb_mutex;               // semaphore for the frame number
pthread_mutex_t video_buffer_mutex;             // semaphore for the video buffer
pthread_cond_t roomAvailable, dataAvailable;    // condition variables to signal availability of room and data in the buffer

void * buffer_ptrs[BUFFER_DIM];         // array of pointers to the buffers
size_t buffer_lengths[BUFFER_DIM];      // array of lengths of the buffers

/**
 * @brief Structure that defines the buffer to store the frames
 * @param head head of the buffer
 * @param tail tail of the buffer
 * @param height height of the frame
 * @param width width of the frame
 * @param frame_size size of the frame
 * @param format the type (MJPG or YUYV)
 * @param buffer buffer to store the frames
 */
struct BufferData {
    int head;
    int tail;
    int height;
    int width;
    int frame_size;
    char format[4];
    unsigned char buffer[];
};

char * usage_string = "Usage: %s <format> <height> <width> <framerate> <timeout> <num threads>\n";
int vd, timer;
int frame_numb = 0;
int finish;
time_t start;

struct BufferData * sharedBuf;      // shared buffer

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
 * @brief Function that consumes the frames from the buffer and saves them on the disk
 * @param arg argument
 * @return
 */
static void * frame_consumer(void * arg){
    
    char type[4];
    char filename[20];
    int fr_numb = 0;

    while(1){        

        // critical section
        pthread_mutex_lock(&buf_mutex);
        while(sharedBuf->head == sharedBuf->tail){
            // termination condition
            pthread_mutex_lock(&fin_mutex);
            if(finish == 0){
                pthread_mutex_unlock(&fin_mutex);
                pthread_mutex_unlock(&buf_mutex);
                return NULL;  // exit the thread
            }
            pthread_mutex_unlock(&fin_mutex);
            pthread_cond_wait(&dataAvailable, &buf_mutex);
            // end termination condition
        }
        
        unsigned char frame [sharedBuf->frame_size];
        strcpy(type, sharedBuf->format);
        int h = sharedBuf->height;
        int w = sharedBuf->width;
        //collect the frame from the buffer
        memcpy(frame, &sharedBuf->buffer[sharedBuf->head * sharedBuf->frame_size], sharedBuf->frame_size);
        sharedBuf->head = (sharedBuf->head + 1) % BUFFER_SIZE;      // circular array
        pthread_cond_signal(&roomAvailable);
        pthread_mutex_unlock(&buf_mutex);
        // end critical section

        // save the frame in the disk (folder frame)
        pthread_mutex_lock(&frame_numb_mutex);
        fr_numb = frame_numb;
        frame_numb ++;
        pthread_mutex_unlock(&frame_numb_mutex);

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
    }
}

/**
 * @brief Function that get the frames from the webcam and stores them in the buffer
 * @param arg argument
 * @return
 */
static void * frame_producer(void * arg){ 

    struct v4l2_buffer buf;    // buffer structure

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // loop until the timer expires
    while(difftime(time(0), start) < timer){

        // check if there is room to store the frame
        pthread_mutex_lock(&buf_mutex);
        while(sharedBuf->head == (sharedBuf->tail + 1) % BUFFER_SIZE){
            // no room available
            perror("Buffer is full\n");
            pthread_cond_wait(&roomAvailable, &buf_mutex);

        }

        // extract the frame from the buffer
        pthread_mutex_lock(&video_buffer_mutex);
        if(ioctl(vd, VIDIOC_DQBUF, &buf) == -1){
            perror("Dequeue buffer error");
            // pthread_mutex_unlock(&video_buffer_mutex);
            // continue;
        }
        pthread_mutex_unlock(&video_buffer_mutex);

        // critical section
        unsigned char * buf_frame = &sharedBuf->buffer[sharedBuf->tail * sharedBuf->frame_size];
        memcpy(buf_frame, buffer_ptrs[buf.index], sharedBuf->frame_size);
        sharedBuf->tail = (sharedBuf->tail + 1) % BUFFER_SIZE;
        pthread_cond_signal(&dataAvailable);
        pthread_mutex_unlock(&buf_mutex);
        //end critical section

        // re-enqueue the buffer
        pthread_mutex_lock(&video_buffer_mutex);
        if(ioctl(vd, VIDIOC_QBUF, &buf) == -1){
            perror("Queue buffer error");
            pthread_mutex_unlock(&video_buffer_mutex);
            continue;
        }
        pthread_mutex_unlock(&video_buffer_mutex);
    }

    pthread_mutex_lock(&fin_mutex);
    finish --;      // one producer has finished
    printf("Producer finished: %d\n", finish);
    pthread_mutex_unlock(&fin_mutex);

    pthread_cond_broadcast(&dataAvailable);    // wake up ALL threads

    return NULL;
}

/**
 * @brief Main function
 * @param argc number of arguments
 * @param argv array of arguments
 */
int main(int argc, char **argv){

    int height, width, frame_size;
    int n_threads;                          // number of threads (consumer = producer)

    struct v4l2_capability cap;                                 // capability structure
    struct v4l2_format fmt;                                     // format structure
    struct v4l2_streamparm streamparm;                          // stream parameter structure
    struct v4l2_requestbuffers req;                             // request buffer structure
    struct v4l2_buffer buf;                                     // buffer structure
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;      // buffer type


    if(argc == 1 || argc != 7){ 
        printf(usage_string, argv[0]); 
        exit(EXIT_FAILURE);
    }

    printf("Format: %s - Height: %d - Width: %d - Frame Rate: %d - Timeout: %d - N_Threads: %d\n", argv[1], atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));

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

    // check the timeout value
    if(atoi(argv[5]) < 0){
        printf("Timeout must be a positive value\n");
        exit(EXIT_FAILURE);
    }else{
        printf("Timeout: %d\n", atoi(argv[5]));
        timer = atoi(argv[5]);
    }

    // check number of threads
    if(atoi(argv[6]) < 0){
        printf("Number of threads must be a positive value\n");
        exit(EXIT_FAILURE);
    }else{
        printf("Number of threads: %d\n", atoi(argv[6]));
        n_threads = atoi(argv[6]);
        finish = atoi(argv[6]);
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

    height = fmt.fmt.pix.height;
    width = fmt.fmt.pix.width;

    if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV){
        frame_size = height * width * 2;    // 2 bytes for each pixel
    }else if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG){
        frame_size = height * width * 3;    // 3 bytes for each pixel
    }

    printf("Frame Size: %d\n", frame_size);

    // buffer videocamera setting
    
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

    // shared buffer settings
    sharedBuf = malloc(sizeof(struct BufferData) + BUFFER_SIZE * frame_size);
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

    // semaphore settings
    pthread_mutex_init(&buf_mutex, NULL);
    pthread_mutex_init(&fin_mutex, NULL);
    pthread_mutex_init(&frame_numb_mutex, NULL);
    pthread_mutex_init(&video_buffer_mutex, NULL);
    pthread_cond_init(&roomAvailable, NULL);
    pthread_cond_init(&dataAvailable, NULL);

    // start the producer and consumer
    pthread_t producers[n_threads], consumers[n_threads];
    int thread_ids[n_threads];

    // start the streaming
    if(ioctl(vd, VIDIOC_STREAMON, &type) == -1){
        perror("Streaming start error");
        exit(EXIT_FAILURE);
    }

    start = time(0);    // get the current time

    for (int i = 0; i < n_threads; i++) {
        thread_ids[i] = i;
        pthread_create(&producers[i], NULL, frame_producer, &thread_ids[i]);
        pthread_create(&consumers[i], NULL, frame_consumer, &thread_ids[i]);
    }

    // wait for the threads to finish
    for (int i = 0; i < n_threads; i++) {
        pthread_join(producers[i], NULL);
        pthread_join(consumers[i], NULL);
    }

    // stop streaming
    if(ioctl(vd, VIDIOC_STREAMOFF, &type) == -1){
        perror("Streaming stop error");
        exit(EXIT_FAILURE);
    }

    printf("End acquisition\n");

    // free memory
    free(sharedBuf);
    for(int i = 0; i < BUFFER_DIM; i++){
        munmap(buffer_ptrs[i], buffer_lengths[i]);
    }
   
    // close the device (videocamera)
    close(vd);
    printf("Close the videocamera\n");

    return 0;
}