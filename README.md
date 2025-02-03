# CONCURRENT AND REAL TIME PROGRAMMING - REPOSITORY TEMPLATE #
Concurrent and Real Time Project - 2024/25 

This repository is a template repository for the homeworks to be developed in the [Concurrent and Real Time Programming](https://stem.elearning.unipd.it/course/view.php?id=10492) course.

*Concurrent and Real Time Programming* is a course of the [Master Degree in Computer Engineering](https://degrees.dei.unipd.it/master-degrees/computer-engineering/) of the  [Department of Information Engineering](https://www.dei.unipd.it/en/), [University of Padua](https://www.unipd.it/en/), Italy.

## Exercise ##
**Exercise 01:** *Acquisition of frames from a Webcam in Linux and storage of the frames on disk. Variation in disk write shall not affect the frequency at which frames are acquired. Therefore the task carrying out frame write on disk shall be decoupled from the task supervising frames acquisition. Task implementation can be via Linux processes or Threads.*

## Camera settings ##

`ioctl: VIDIOC_ENUM_FMT`

        Type: Video Capture

        [0]: 'MJPG' (Motion-JPEG, compressed)
        [1]: 'YUYV' (YUYV 4:2:2)

`Streaming Parameters Video Capture:`

        Capabilities     : timeperframe
        Frames per second: 30.000 (30/1)
        Read buffers     : 0

## Organisation of the repository ##
The repository is organised as follows:
* code: folder that contains all the code for the project + files for git.
    * frame: folder that contains all the frames captured by the program.

## STRUCTURE ##

To define


### License ###

All the contents of this repository are shared using the [Creative Commons Attribution-ShareAlike 4.0 International License](http://creativecommons.org/licenses/by-sa/4.0/).

![CC logo](https://i.creativecommons.org/l/by-sa/4.0/88x31.png)
