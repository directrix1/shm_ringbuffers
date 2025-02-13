/******************************************************************************
 *
 * Copyright (c) 2025-present Edward Andrew Flick.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include <bits/time.h>
#include <math.h>
#include <shm_ringbuffers.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

SRBHandle h = NULL;

double get_cur_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec + (double)ts.tv_nsec / 1000000000.0);
}

void closeSRB(int signum)
{
    printf("Closing shared buffers (%d).\n", signum);
    srb_close(h);
    exit(0);
}

#pragma pack(1)
struct RGBA {
    unsigned char r, g, b, a;
};

void make_frame(struct RGBA* pixels, double t)
{
    // Make a pretty animated pattern in the buffer
    int tt = t * 128.0;
    for (int y = -540; y < 540; y++) {
        for (int x = -960; x < 960; x++) {
            int d = abs(x) + abs(y);
            pixels->r = ((d * 1) + tt) % 255;
            pixels->g = ((d * 2) + tt) % 255;
            pixels->b = ((d * 3) + tt) % 255;
            pixels->a = 255;
            pixels++;
        }
    }
}

#define FPS (60)

int main(__attribute((unused)) int argc, __attribute((unused)) char** argv)
{
    char* channelName = "video_frames";
    struct ShmRingBuffer* srb;

    // Host needs to be run before and while all clients are run.
    printf("Producing: %s at /srb_video_test\n", channelName);

    h = srb_client_new("/srb_video_test");
    if (h == NULL) {
        printf("Can't connect to /srb_video_test, please run the following command in the background before using this:\n\n srbhost /srb_video_test video_frames 8294400 10\n");
        return 1;
    }
    signal(SIGINT, closeSRB);

    srb = srb_get_ring_by_description(h, channelName);

    double curTime = get_cur_time();
    struct RGBA* pixels;
    double secsPerFrame = 1.0 / FPS;
    long long int frame = 0;
    curTime = get_cur_time();
    while (1) {
        pixels = (struct RGBA*)srb_producer_next_write_buffer(srb);
        double startTime = get_cur_time();
        make_frame(pixels, startTime);
        frame++;
        if (frame == FPS) {
            double newTime = get_cur_time();
            double dt = newTime - curTime;
            curTime = newTime;
            printf("FPS (%d expected): %.2f\n", (FPS), (FPS / dt));
            frame = 0;
        }
        double endTime = get_cur_time();
        double dt = endTime - startTime;
        if (dt < secsPerFrame) {
            dt = secsPerFrame - dt;
            usleep(dt * 1000000);
        }
    };

    return 0;
}
