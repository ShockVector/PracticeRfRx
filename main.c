#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <hackrf.h>
#include <liquid/liquid.h>
#include <complex.h>

volatile sig_atomic_t keep_running = 1;
FILE* outputFile = NULL;

// Error checking macro so that Error checking doesnt clutter main
// it takes the result , the device pointer, and the action to 
// perform in the event that the hackrf_exit() fails
#define Error_CHECK(result,device,action){\
    if(result != HACKRF_SUCCESS){\
        fprintf(stderr,"Error: %s\n",hackrf_error_name(result));\
        if(device != NULL){\
            hackrf_close(device);\
        }\
        hackrf_exit();\
        action;\
    }\
}\

//handle the user input of ctrl C
void sigint_handler(int sig){
    keep_running =0; 
    printf("\n cleaning up and exiting....\n");
}

//rx callback function. called repeatedly on rx start
int rx_callback(hackrf_transfer* transfer){

    //receive the initial set of interleaved complex samples [real][imag][real][imag]....[samples*2]
        size_t num_samples = transfer->valid_length / 2;

        float complex * complex_samples = (float complex *) malloc(num_samples * sizeof(float complex));
        if(complex_samples == NULL){
            fprintf(stderr, "Memory allocation failed\n");
            return -1;
        }

    //process those samples so rather than being interleaved they are in a format that liquid DSP can work with
        for(unsigned int i=0; i< num_samples; i++){
            float real = (float)transfer->buffer[2*i] / 128.0f;
            float imag = (float)transfer->buffer[2*i+1] / 128.0f; 
            complex_samples[i] = real + imag *I;
        }

    // resample the samples to a frequency that we can use for Audio. If we are sampling an FM station it has
    // a bandwidth of 200Khz that means we need to take 200000 samples per second for Audio playback which 
    // should have a playback speed of about 32Khz. The ratio between your interpolation factor and 
    // your decimation factor should be aproximatly (your desired sample rate) / (your current sample rate)

        unsigned int interpFactor = 16; // interpolation factor
        unsigned int decFactor = 100; // decimation factor

        //Black magic, not sure what these do except r
        unsigned int h_len = 13;    // filter semi-length (filter delay)
        float r=(float)interpFactor/(float)decFactor; // resampling rate (output/input)
        float bw=0.45f;              // resampling filter bandwidth
        float slsl=60.0f;          // resampling filter sidelobe suppression level
        unsigned int npfb=32;       // number of filters in bank (timing resolution)

        
        resamp_crcf resampler = resamp_crcf_create(r,h_len,bw,slsl,npfb);//create an arbitrary resampler object
        size_t num_output_samples = (size_t)((double)num_samples * interpFactor / decFactor)+ 5; 
        float complex *output_samples = (float complex *) malloc(num_output_samples * sizeof(float complex));
        unsigned int nw;
        for(size_t i=0, j =0; i< num_samples; i++){
            resamp_crcf_execute(resampler,complex_samples[i],&output_samples[j],&nw);
            j+=nw;
        }
    //demodulation. here we will demodulate our resampled complex samples. we demodulate after resampling because otherwise
    // it will cause issues. After demodulation we will have the raw audio samples. 
        float modIndex = 5.0f;
        freqdem demod = freqdem_create(modIndex);
        float *audioSamples = (float *)malloc(num_output_samples * sizeof(float));
        if(audioSamples == NULL){
            fprintf(stderr, "Memory allocation failed for audio samples\n");

            freqdem_destroy(demod);
            free(output_samples);
            return -1;
        }
        for (size_t i=0; i<num_output_samples; i++){
            freqdem_demodulate(demod, output_samples[i], &audioSamples[i]);
            printf("Audio Sample %zu: %f\n", i, audioSamples[i]);
        }

    //write the samples to the output file
        fwrite(audioSamples, sizeof(float), num_output_samples, outputFile);

    //cleanup and return
        resamp_crcf_destroy(resampler);
        freqdem_destroy(demod);
        free(complex_samples);
        free(output_samples);
        return 0;
}

//main function handles the creation of the hackrf instance as well as
//signal registration and cleanup
int main(){

    //register signals

        signal(SIGINT, sigint_handler);

    // initialize the hackrf pointer
        int result;
        hackrf_device* device = NULL;

    //initialize the hackrf device
        result = hackrf_init();
        Error_CHECK(result,NULL,return EXIT_FAILURE);

    //open a connection to the hackrf
        result = hackrf_open(&device);
        Error_CHECK(result,NULL,return EXIT_FAILURE);

    //set the center frequency in Hz that you are focused on. the signal will vary around this frequency by
    // +/- whatever the bandwidth is. You will need to appropriatly match your antenna to the frequency you are recording. 
    // troubleshooting tip. use SDR# (windows) or GQRX to verify that you are actually receiving your signal. 
        uint64_t freq = 98706800;
        result = hackrf_set_freq(device,freq);
        Error_CHECK(result,device,return EXIT_FAILURE);

    //set the sample rate and check for error. The sample rate you will need is determined by
    // the bandwidth of the signal you are collecting. This is due to the SDR shifting the signal to 
    // baseband (0Hz). The collected signal now has a real component I and an imaginary component Q(you cant have a negative frequency). So 
    // your sample rate is now equal to your  bandwidth Ex 20MHz bandwidth requires 20M samples/ sec.
    // More is usually uneccisary and less will cause aliasing. 
        uint32_t sample_rate = 200000;
        result = hackrf_set_sample_rate(device, sample_rate);
        Error_CHECK(result,device, return EXIT_FAILURE);
    //open the output file

    outputFile = fopen("outputAudio.dat","wb");
    //start the hackrf rx and check for error
        result = hackrf_start_rx(device,rx_callback,NULL);
        Error_CHECK(result,device,return EXIT_FAILURE);

    //loopback to keep the application running until the user shuts it down
        while (keep_running == 1){}

    //clean up
        hackrf_close(device);
        hackrf_exit();
        fclose(outputFile);
        printf("Done.\n");
        return EXIT_SUCCESS;

}
