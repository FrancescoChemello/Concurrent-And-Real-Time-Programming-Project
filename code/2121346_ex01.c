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
#include <linux/videodev2.h>    // for v4l2
#include <sys/ioctl.h>          // for input/output control
#include <sys/ipc.h>            // for inter-process communication
#include <sys/msg.h>            // for message queue
#include <sys/mman.h>           // for memory map
#include <time.h>               // for the timer
#include <sys/wait.h>           // for waitpid

#define DEVICE "/dev/video0"
#define BUFFER_SIZE 100
#define BUFFER_DIM 30
#define PRODCONS_TYPE 1

struct msgframe{
    long mtype;                 // message type (required for message queue)
    int frame_size;             // size of the frame
    char type[5];               // the type (MJPG or YUYV) or EOFT (End OF Transmission)
    char shm_name[50];          // memory map name
};

void *buffer_ptrs[BUFFER_DIM];          // array of pointers to the buffers
size_t buffer_lengths[BUFFER_DIM];      // array of lengths of the buffers

char * usage_string = "Usage: %s <format> <height> <width> <framerate> <acquisitiontime>\n";
int vd, timer;
int msgId;      // message queue id

/**
 * @brief Function that frees the memory and closes the device
 * @param
 * @return
 */
void cleanup() {
    // Free memory
    for(int i = 0; i < BUFFER_DIM; i++){
        if (buffer_ptrs[i] != MAP_FAILED && buffer_ptrs[i] != NULL) {
            munmap(buffer_ptrs[i], buffer_lengths[i]);
        }
    }

    // Close the device (videocamera)
    if (vd != -1) {
        close(vd);
    }

    // Remove the message queue
    if (msgId != -1) {
        msgctl(msgId, IPC_RMID, NULL);
    }
}

/**
 * @brief Function that consumes a frame and stores it in the disk
 * @param 
 * @return
 */
static void frame_consumer(){

    char type[4];
    char filename[20];
    int frame_numb = 0;
    struct msgframe msg;
    int recv;

    while(1){

        // receive the message
        recv = msgrcv(msgId, &msg, sizeof(msg), 0, 0);

        if(recv == -1){
            perror("Error receiving message");
            continue;
        }

        // ending condition
        if (strcmp(msg.type, "EOFT") == 0){
            break;      // End OF Transmission message received
        }

        // process the frame
        int shm_fd = shm_open(msg.shm_name, O_RDONLY, 0666);
        unsigned char * frame = mmap(NULL, msg.frame_size, PROT_READ, MAP_SHARED, shm_fd, 0);
        
        // check if the memory map is created
        if (frame == MAP_FAILED) {
            perror("MMAP error");
            cleanup();
            exit(EXIT_FAILURE);
        }

        // save the frame in the disk (folder frame)
        if(strncmp(msg.type, "YUYV", 4) == 0){
            sprintf(filename, "frame/frame_%d.raw", frame_numb);        // save the frame in .raw
        }else{
            sprintf(filename, "frame/frame_%d.jpg", frame_numb);        // save the frame in .jpg
        }
        FILE * frame_file = fopen(filename, "wb");
        if(frame_file){
            fwrite(frame, 1, msg.frame_size, frame_file);
            fclose(frame_file);
        }else{
            perror("Error saving the frame\n");
        }
        frame_numb++;

        // free memory map
        munmap(frame, msg.frame_size);
        shm_unlink(msg.shm_name);
    }
}

/**
 * @brief Function that produces a frame and sends it to the consumer
 * @param frame_size size of the frame
 * @param type type of the frame (MJPG or YUYV)
 * @return
 */
static void frame_producer(int frame_size, char frtype[5]){   

    struct v4l2_buffer buf;     // buffer structure
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    // start the streaming
    if(ioctl(vd, VIDIOC_STREAMON, &type) == -1){
        perror("Start streaming error");
        cleanup();
        exit(EXIT_FAILURE);
    }

    struct msgframe msg;        // message
    msg.mtype = PRODCONS_TYPE;  // message type

    time_t start = time(0);    // get the current time
    int frame_numb = 0;

    // loop until the timer expires
    while(difftime(time(0), start) < timer){

        // extract the frame from the buffer
        if(ioctl(vd, VIDIOC_DQBUF, &buf) == -1){
            perror("Dequeue buffer error");
            continue;
        }

        // cration of memory map
        sprintf(msg.shm_name, "/frame_%d", frame_numb);
        
        int memsh = shm_open(msg.shm_name, O_CREAT | O_RDWR, 0666);   
        
        if(memsh == -1){
            perror("Shared memory creation failed");
            continue;
        }

        // check if I set the correct size of memory (= total_size)
        if(ftruncate(memsh, frame_size) == -1){
            perror("ftruncate error");
            cleanup();
            exit(EXIT_FAILURE);
        }

        unsigned char * mmap_frame = mmap(NULL, frame_size, PROT_READ | PROT_WRITE, MAP_SHARED, memsh, 0);

        // check if I create the shared memory
        if(mmap_frame == MAP_FAILED){
            perror("Mmap failed");
            continue;
        }

        // creation of the message

        // copy the frame into the memory map
        memcpy(mmap_frame, buffer_ptrs[buf.index], frame_size);
        msg.frame_size = frame_size;
        strncpy(msg.type, frtype, sizeof(msg.type));

        
        // send message to frame_consumer
        if(msgsnd(msgId, &msg, sizeof(msg) - sizeof(long), 0) == -1){
            perror("Error sending message");
            continue;
        }

        // re-enqueue the buffer
        if(ioctl(vd, VIDIOC_QBUF, &buf) == -1){
            perror("Queue buffer error");
            continue;
        }

        frame_numb++;
    }

    // stop streaming
    if(ioctl(vd, VIDIOC_STREAMOFF, &type) == -1){
        perror("Stop streaming error");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // send the end of transmission message
    strcpy(msg.type, "EOFT");
    msg.frame_size = 0;
    msgsnd(msgId, &msg, sizeof(msg) - sizeof(long), 0);

    printf("Sent %d packets\n", frame_numb);
}

/**
 * @brief Main function
 * @param argc number of arguments
 * @param argv array of arguments
 */
int main(int argc, char **argv){
    int status, memsh, fin;
    int frame_size, height, width;
    char type [5];
    
    struct v4l2_capability cap;             // capability structure
    struct v4l2_format fmt;                 // format structure
    struct v4l2_streamparm streamparm;      // stream parameter structure
    struct v4l2_requestbuffers req;         // request buffer structure
    struct v4l2_buffer buf;                 // buffer structure

    if(argc == 1){ 
        printf(usage_string, argv[0]); 
        exit(EXIT_FAILURE);
    }

    printf("Format: %s - Width: %d - Height: %d - Frame Rate: %d - Timeout: %d\n", argv[1], atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));

    // check if it can be opened
    vd = open(DEVICE, O_RDWR);
    if(vd == -1){
        perror("Error opening device");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // check if it supports querying capabilities
    if(ioctl(vd, VIDIOC_QUERYCAP, &cap) == -1){
        perror("Error querying capability");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // check if it supports streaming
    if(!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        printf("Streaming NOT supported\n");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // videocamera setting negotiation 

    // initialize the format structure
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    // setting the format
    if(!strcmp(argv[1],"MJPG") || !strcmp(argv[1],"mjpg")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        strcpy(type, "MJPG\0");
    } else if (!strcmp(argv[1],"YUYV") || !strcmp(argv[1],"yuyv")){
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        strcpy(type, "YUYV\0");
    } else {
        printf("Format %s not supported\n", argv[1]);
        cleanup();
        exit(EXIT_FAILURE);
    }

    // setting the frame size
    fmt.fmt.pix.width = atoi(argv[2]);
    fmt.fmt.pix.height = atoi(argv[3]);
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    // check if it supports the format
    if(ioctl(vd, VIDIOC_S_FMT, &fmt) == -1){
        perror("Error setting format");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // check the format accepted
    printf("Accepted format: %c%c%c%c, %dx%d\n", 
        fmt.fmt.pix.pixelformat & 0xFF, 
        (fmt.fmt.pix.pixelformat >> 8) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 16) & 0xFF,
        (fmt.fmt.pix.pixelformat >> 24) & 0xFF,
        fmt.fmt.pix.width, fmt.fmt.pix.height);

    frame_size = (unsigned int)fmt.fmt.pix.sizeimage;
    
    // setting frame rate
    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator = 1;
    streamparm.parm.capture.timeperframe.denominator = atoi(argv[4]);
    
    // check if it supports the frame rate
    if(ioctl(vd, VIDIOC_S_PARM, &streamparm) == -1){
        perror("Error setting frame rate");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // check the frame rate accepted
    printf("Frame rate accepted: %d/%d FPS\n",
        streamparm.parm.capture.timeperframe.denominator,
        streamparm.parm.capture.timeperframe.numerator);

    // check the timeout value
    if(atoi(argv[5]) < 0){
        printf("Acquisition time must be a positive value\n");
        cleanup();
        exit(EXIT_FAILURE);
    }else{
        printf("Timeout: %d\n", atoi(argv[5]));
        timer = atoi(argv[5]);
    }

    // definition of the message
    msgId = msgget(IPC_PRIVATE, 0666);

    if(msgId == -1){
        perror("Error creating the message queue");
        cleanup();
        exit(EXIT_FAILURE);
    }

    printf("Message queue created \n");

    // set the structure for the request
    memset(&req, 0, sizeof(req));               // set to 0 the structure
    req.count = BUFFER_DIM;                     // set the number of buffers
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;     // set the type of buffer (video capture)
    req.memory = V4L2_MEMORY_MMAP;              // set the memory type (mmap)

    // check if the request is accepted
    if(ioctl(vd, VIDIOC_REQBUFS, &req) == -1){
        perror("VIDIOC_REQBUFS error");
        cleanup();
        exit(EXIT_FAILURE);
    }

    // configuration of the buffers
    for(int i = 0; i < BUFFER_DIM; i++){

        memset(&buf, 0, sizeof(buf));               // set to 0 the structure
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;     // set the type of buffer (video capture)
        buf.memory = V4L2_MEMORY_MMAP;              // set the memory type (mmap)
        buf.index = i;                              // set the index of the buffer

        // check if the buffer is accepted
        if(ioctl(vd, VIDIOC_QUERYBUF, &buf) == -1){
            perror("VIDIOC_QUERYBUF error");
            cleanup();
            exit(EXIT_FAILURE);
        }

        // map the buffer in memory (mmap)
        buffer_ptrs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, vd, buf.m.offset);
        if(buffer_ptrs[i] == MAP_FAILED){
            perror("MMAP failure");
            cleanup();
            exit(EXIT_FAILURE);
        }

        buffer_lengths[i] = buf.length;             // set the length of the buffer

        // enqueue the buffer
        if(ioctl(vd, VIDIOC_QBUF, &buf) == -1){
            perror("VIDIOC_QBUF error");
            cleanup();
            exit(EXIT_FAILURE);
        }
    }

    printf("Configuration of the buffers completed\n");

    // start the producer and consumer
    printf("Start acquisition\n");
    
    pid_t pid;
    pid = fork();

    if(pid < 0){
        perror("fork error");
        cleanup();
        exit(EXIT_FAILURE);
    }
    if(pid == 0){
        frame_consumer();
        exit(0);
    }
    
    frame_producer(frame_size, type);

    // wait the consumer
    waitpid(pid, NULL, 0);
    printf("End acquisition\n");

    // free memory and close the device (videocamera)
    cleanup();
    
    return 0;
}