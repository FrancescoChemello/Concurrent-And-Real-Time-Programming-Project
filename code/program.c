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
#include <string.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>

#define DEVICE "/dev/video0"
#define MAX_FORMAT 100
#define CHECK_IOCTL_STATUS(message)

char * usage_string = "Usage: %s <format> <height> <width> <framerate> <timeout>\n";

int main(int argc, char **argv){
    int fd, status, idx;
    
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

    
    return 0;
}