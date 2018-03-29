// ec - echo canceller

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#include <speex/speex_echo.h>

#include <audio.h>

const char *usage =
    "Usage:\n %s [options]\n"
    "Options:\n"
    " -i PCM            playback PCM (default)\n"
    " -o PCM            capture PCM (default)\n"
    " -r rate           sample rate (16000)\n"
    " -c channels       recording channels (2)\n"
    " -b size           buffer size (262144)\n"
    " -d delay          system delay between playback and capture (0)\n"
    " -f filter_length  AEC filter length (2048)\n"
    " -s                save audio to /tmp/playback.raw, /tmp/recording.raw and /tmp/out.raw\n"
    " -D                daemonize\n"
    " -h                display this help text\n"
    "Note:\n"
    " Access audio I/O through named pipes (/tmp/ec.input for playback and /tmp/ec.output for recording)\n"
    "  `cat audio.raw > /tmp/ec.input` to play audio\n"
    "  `cat /tmp/ec.output > out.raw` to get recording audio\n"
    " Only support mono playback\n";

volatile int g_is_quit = 0;

extern char *playback_fifo;
extern char *capture_fifo;

extern int fifo_setup(PaUtilRingBuffer *playback, PaUtilRingBuffer *capture);

void int_handler(int signal)
{
    printf("Caught signal %d, quit...\n", signal);

    g_is_quit = 1;
}

int main(int argc, char *argv[])
{
    SpeexEchoState *echo_state;
    int16_t *near = NULL;
    int16_t *far = NULL;
    int16_t *out = NULL;
    FILE *fp_near = NULL;
    FILE *fp_far = NULL;
    FILE *fp_out = NULL;

    int opt = 0;
    int input_channels = 2;
    int output_channels = 1;
    int sample_rate = 16000;
    int buffer_size = 1024 * 16;
    int delay = 0;
    int filter_length = 1024 * 2;
    int save_audio = 0;
    int daemonize = 0;

    while ((opt = getopt(argc, argv, "b:c:d:Df:hi:o:r:s")) != -1)
    {
        switch (opt)
        {
        case 'b':
            buffer_size = atoi(optarg);
            break;
        case 'c':
            input_channels = atoi(optarg);
            break;
        case 'd':
            delay = atoi(optarg);
            break;
        case 'D':
            daemonize = 1;
            break;
        case 'f':
            filter_length = atoi(optarg);
            break;
        case 'h':
            printf(usage, argv[0]);
            exit(0);
        case 'i':
            g_capture_device = optarg;
            break;
        case 'o':
            g_playback_device = optarg;
            break;
        case 'r':
            sample_rate = atoi(optarg);
            break;
        case 's':
            save_audio = 1;
            break;
        case '?':
            printf("\n");
            printf(usage, argv[0]);
            exit(1);
        default:
            break;
        }
    }

    if (daemonize)
    {
        pid_t pid, sid;

        /* Fork off the parent process */
        pid = fork();
        if (pid < 0)
        {
            printf("fork() failed\n");
            exit(1);
        }
        /* If we got a good PID, then
           we can exit the parent process. */
        if (pid > 0)
        {
            exit(0);
        }

        /* Change the file mode mask */
        umask(0);

        /* Open any logs here */

        /* Create a new SID for the child process */
        sid = setsid();
        if (sid < 0)
        {
            printf("setsid() failed\n");
            exit(1);
        }

        /* Change the current working directory */
        if ((chdir("/")) < 0)
        {
            printf("chdir() failed\n");
            exit(1);
        }
    }

    int frame_size = sample_rate * 10 / 1000; // 10 ms

    if (save_audio)
    {
        fp_far = fopen("/tmp/playback.raw", "wb");
        fp_near = fopen("/tmp/recording.raw", "wb");
        fp_out = fopen("/tmp/out.raw", "wb");

        if (fp_far == NULL || fp_near == NULL || fp_out == NULL)
        {
            printf("Fail to open file(s)\n");
            exit(1);
        }
    }

    near = (int16_t *)calloc(frame_size * input_channels, sizeof(int16_t));
    far = (int16_t *)calloc(frame_size * output_channels, sizeof(int16_t));
    out = (int16_t *)calloc(frame_size * input_channels, sizeof(int16_t));

    if (near == NULL || far == NULL || out == NULL)
    {
        printf("Fail to allocate memory\n");
        exit(1);
    }

    // Configures signal handling.
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = int_handler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);

    echo_state = speex_echo_state_init_mc(frame_size, filter_length, input_channels, output_channels);
    speex_echo_ctl(echo_state, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate);

    fifo_setup(&g_ringbuffer[PLAYBACK_INDEX], &g_ringbuffer[PROCESSED_INDEX]);

    audio_start(sample_rate, input_channels, buffer_size);

    printf("Running... Press Ctrl+C to exit\n");

    int wait_us = frame_size * 1000000 / sample_rate / 2;

    // system delay between recording and playback
    while (PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[CAPTURE_INDEX]) < delay)
    {
        usleep(wait_us);
    }
    PaUtil_AdvanceRingBufferReadIndex(&g_ringbuffer[CAPTURE_INDEX], delay);

    while (!g_is_quit)
    {
        while (!g_is_quit && PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[CAPTURE_INDEX]) < frame_size)
        {
            usleep(wait_us);
        }
        PaUtil_ReadRingBuffer(&g_ringbuffer[CAPTURE_INDEX], near, frame_size);

        while (!g_is_quit && PaUtil_GetRingBufferReadAvailable(&g_ringbuffer[PLAYED_INDEX]) < frame_size)
        {
            usleep(wait_us);
        }
        PaUtil_ReadRingBuffer(&g_ringbuffer[PLAYED_INDEX], far, frame_size);

        speex_echo_cancellation(echo_state, near, far, out);

        if (fp_far) {
            fwrite(near, 2, frame_size * input_channels, fp_near);
            fwrite(far, 2, frame_size, fp_far);
            fwrite(out, 2, frame_size * input_channels, fp_out);
        }

        PaUtil_WriteRingBuffer(&g_ringbuffer[PROCESSED_INDEX], out, frame_size);
    }

    if (fp_far) {
        fclose(fp_near);
        fclose(fp_far);
        fclose(fp_out);
    }

    free(near);
    free(far);
    free(out);

    audio_stop();

    exit(0);

    return 0;
}
