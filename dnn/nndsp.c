#include "nndsp.h"
#include "arch.h"
#include "nnet.h"
#include "os_support.h"

#include <math.h>

#define SET_ZERO(x) memset(x, 0, sizeof(x))
#define KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel) ((((i_out_channels) * in_channels) + (i_in_channels)) * kernel_size + (i_kernel))

void init_adaconv_state(AdaConvState *hAdaConv)
{
    memset(hAdaConv, 0, sizeof(*hAdaConv));
}

void init_adacomb_state(AdaCombState *hAdaComb)
{
    memset(hAdaComb, 0, sizeof(*hAdaComb));
}

#ifdef DEBUG_NNDSP
void print_float_vector(const char* name, float *vec, int length)
{
    for (int i = 0; i < length; i ++)
    {
        printf("%s[%d]: %f\n", name, i, vec[i]);
    }
}
#endif

static void scale_kernel(
    float *kernel,
    int in_channels,
    int out_channels,
    int kernel_size,
    float p,
    float *gain
)
/* normalizes (p-norm) kernel over input channel and kernel dimension */
{
    float norm;
    int i_in_channels, i_out_channels, i_kernel;

    for (i_out_channels = 0; i_out_channels < out_channels; i_out_channels++)
    {
        norm = 0;
        for (i_in_channels = 0; i_in_channels < in_channels; i_in_channels ++)
        {
            for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
            {
                norm += pow(kernel[KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel)], p);
            }
        }
#ifdef DEBUG_NNDSP
        printf("kernel norm: %f, %f\n", norm, pow(norm, 1.f/p));
#endif
        norm = 1. / (1e-6 + pow(norm, 1.f/p));
        for (i_in_channels = 0; i_in_channels < in_channels; i_in_channels++)
        {
            for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
            {

                kernel[KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel)] *= norm * gain[i_out_channels];
            }
        }
    }
}

static void transform_gains(
    float *gains,
    int num_gains,
    float filter_gain_a,
    float filter_gain_b
)
{
    int i;
    for (i = 0; i < num_gains; i++)
    {
        gains[i] = exp(filter_gain_a * gains[i] + filter_gain_b);
    }
}

void adaconv_process_frame(
    AdaConvState* hAdaConv,
    float *x_out,
    float *x_in,
    float *features,
    LinearLayer *kernel_layer,
    LinearLayer *gain_layer,
    int feature_dim, // not strictly necessary
    int frame_size,
    int overlap_size,
    int in_channels,
    int out_channels,
    int kernel_size,
    int left_padding,
    float filter_gain_a,
    float filter_gain_b,
    float shape_gain,
    float *window
)
{
    float output_buffer[ADACONV_MAX_FRAME_SIZE * ADACONV_MAX_OUTPUT_CHANNELS];
    float kernel_buffer[ADACONV_MAX_KERNEL_SIZE * ADACONV_MAX_INPUT_CHANNELS * ADACONV_MAX_OUTPUT_CHANNELS];
    float input_buffer[ADACONV_MAX_INPUT_CHANNELS * (ADACONV_MAX_FRAME_SIZE + ADACONV_MAX_KERNEL_SIZE)];
    float window_buffer[ADACONV_MAX_OVERLAP_SIZE];
    float gain_buffer[ADACONV_MAX_OUTPUT_CHANNELS];
    float *p_input;
    //int offset = kernel_size * input_channels;
    int i_in_channels, i_out_channels, i_sample, i_kernel;

    celt_assert(shape_gain == 1);
    celt_assert(left_padding == kernel_size - 1); /* currently only supports causal version. Non-causal version not difficult to implement but will require third loop */
    celt_assert(kernel_size < frame_size);

    SET_ZERO(output_buffer);
    SET_ZERO(kernel_buffer);
    SET_ZERO(input_buffer);

#ifdef DEBUG_NNDSP
    print_float_vector("x_in", x_in, in_channels * frame_size);
#endif

    if (window == NULL)
    {
        for (i_sample=0; i_sample < overlap_size; i_sample++)
        {
            window_buffer[i_sample] = 0.5f + 0.5f * cos(M_PI * (i_sample + 0.5f) / overlap_size);
        }
        window = &window_buffer[0];
    }

    OPUS_COPY(input_buffer, hAdaConv->history, kernel_size * in_channels);
    OPUS_COPY(input_buffer + kernel_size * in_channels, x_in, frame_size * in_channels);
    p_input = input_buffer + kernel_size * in_channels;

    /* calculate new kernel and new gain */
    compute_generic_dense(kernel_layer, kernel_buffer, features, ACTIVATION_LINEAR);
    compute_generic_dense(gain_layer, gain_buffer, features, ACTIVATION_TANH);
#ifdef DEBUG_NNDSP
    print_float_vector("features", features, feature_dim);
    print_float_vector("adaconv_kernel_raw", kernel_buffer, in_channels * out_channels * kernel_size);
    print_float_vector("adaconv_gain_raw", gain_buffer, out_channels);
#endif
    transform_gains(gain_buffer, out_channels, filter_gain_a, filter_gain_b);
    scale_kernel(kernel_buffer, in_channels, out_channels, kernel_size, 2, gain_buffer);

#ifdef DEBUG_NNDSP
    print_float_vector("adaconv_kernel", kernel_buffer, in_channels * out_channels * kernel_size);
    print_float_vector("adaconv_gain", gain_buffer, out_channels);
#endif

    /* calculate overlapping part using kernel from last frame */
    for (i_sample = 0; i_sample < overlap_size; i_sample++)
    {
        for (i_out_channels = 0; i_out_channels < out_channels; i_out_channels++)
        {
            for (i_in_channels = 0; i_in_channels < in_channels; i_in_channels++)
            {
                for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
                {
                    output_buffer[i_sample * out_channels + i_out_channels] +=
                        window[i_sample] * p_input[(i_sample + i_kernel - left_padding) * in_channels + i_in_channels] * hAdaConv->last_kernel[KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel)];
                    output_buffer[i_sample * out_channels + i_out_channels] +=
                        (1 - window[i_sample]) * p_input[(i_sample + i_kernel - left_padding) * in_channels + i_in_channels] * kernel_buffer[KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel)];
                }
            }
        }
    }

    /* calculate non-overlapping part */
    for (i_sample = overlap_size; i_sample < frame_size; i_sample++)
    {
        for (i_out_channels = 0; i_out_channels < out_channels; i_out_channels++)
        {
            for (i_in_channels = 0; i_in_channels < in_channels; i_in_channels++)
            {
                for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
                {
                    output_buffer[i_sample * out_channels + i_out_channels] +=
                        p_input[(i_sample + i_kernel - left_padding) * in_channels + i_in_channels] * kernel_buffer[KERNEL_INDEX(i_out_channels, i_in_channels, i_kernel)];
                }
            }
        }
    }

    OPUS_COPY(x_out, output_buffer, out_channels * frame_size);

#ifdef DEBUG_NNDSP
    print_float_vector("x_out", x_out, out_channels * frame_size);
#endif

    /* buffer update */
    OPUS_COPY(hAdaConv->history, &x_in[(frame_size - kernel_size) * in_channels], kernel_size * in_channels);
    OPUS_COPY(hAdaConv->last_kernel, kernel_buffer, kernel_size * in_channels * out_channels);
}

void adacomb_process_frame(
    AdaCombState* hAdaComb,
    float *x_out,
    float *x_in,
    float *features,
    LinearLayer *kernel_layer,
    LinearLayer *gain_layer,
    LinearLayer *global_gain_layer,
    int pitch_lag,
    int frame_size,
    int overlap_size,
    int kernel_size,
    int left_padding,
    float filter_gain_a,
    float filter_gain_b,
    float log_gain_limit,
    float *window
)
{
    float output_buffer[ADACOMB_MAX_FRAME_SIZE];
    float kernel_buffer[ADACOMB_MAX_KERNEL_SIZE];
    float input_buffer[ADACOMB_MAX_FRAME_SIZE + ADACOMB_MAX_LAG + ADACOMB_MAX_KERNEL_SIZE];
    float window_buffer[ADACOMB_MAX_OVERLAP_SIZE];
    float gain, global_gain;
    float *p_input;
    int i_sample, i_kernel;

    celt_assert(shape_gain == 1);
    celt_assert(left_padding == kernel_size - 1); /* currently only supports causal version. Non-causal version not difficult to implement but will require third loop */

    SET_ZERO(output_buffer);
    SET_ZERO(kernel_buffer);
    SET_ZERO(input_buffer);


    if (window == NULL)
    {
        for (i_sample=0; i_sample < overlap_size; i_sample++)
        {
            window_buffer[i_sample] = 0.5f + 0.5f * cos(M_PI * (i_sample + 0.5f) / overlap_size);
        }
        window = &window_buffer[0];
    }

    OPUS_COPY(input_buffer, hAdaComb->history, kernel_size + ADACOMB_MAX_LAG);
    OPUS_COPY(input_buffer + kernel_size + ADACOMB_MAX_LAG, x_in, frame_size);
    p_input = input_buffer + kernel_size + ADACOMB_MAX_LAG;

    /* calculate new kernel and new gain */
    compute_generic_dense(kernel_layer, kernel_buffer, features, ACTIVATION_LINEAR);
    compute_generic_dense(gain_layer, &gain, features, ACTIVATION_RELU);
    compute_generic_dense(global_gain_layer, &global_gain, features, ACTIVATION_TANH);
#ifdef DEBUG_NNDSP
    print_float_vector("features", features, feature_dim);
    print_float_vector("adacomb_kernel_raw", kernel_buffer, kernel_size);
    print_float_vector("adacomb_gain_raw", &gain, 1);
    print_float_vector("adacomb_global_gain_raw", &global_gain, 1);
#endif
    gain = exp(log_gain_limit - gain);
    global_gain = exp(filter_gain_a * global_gain + filter_gain_b);
    scale_kernel(kernel_buffer, 1, 1, kernel_size, 2, &gain);

#ifdef DEBUG_NNDSP
    print_float_vector("adacomb_kernel", kernel_buffer, kernel_size);
    print_float_vector("adacomb_gain", &gain, 1);
#endif

    /* calculate overlapping part using kernel and lag from last frame */
    for (i_sample = 0; i_sample < overlap_size; i_sample++)
    {
        for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
        {
            output_buffer[i_sample] += hAdaComb->last_global_gain * window[i_sample] * p_input[i_sample + i_kernel - left_padding - hAdaComb->last_pitch_lag] * hAdaComb->last_kernel[i_kernel];
            output_buffer[i_sample] += global_gain * (1 - window[i_sample]) * p_input[i_sample + i_kernel - left_padding - pitch_lag] * kernel_buffer[i_kernel];
        }
        output_buffer[i_sample] += (window[i_sample] * hAdaComb->last_global_gain + (1 - window[i_sample]) * global_gain) * p_input[i_sample];
    }


    /* calculate non-overlapping part */
    for (i_sample = overlap_size; i_sample < frame_size; i_sample++)
    {
        for (i_kernel = 0; i_kernel < kernel_size; i_kernel++)
        {
            output_buffer[i_sample] += p_input[i_sample + i_kernel - left_padding - pitch_lag] * kernel_buffer[i_kernel];
        }
        output_buffer[i_sample] = global_gain * (output_buffer[i_sample] + p_input[i_sample]);
    }

    OPUS_COPY(x_out, output_buffer, frame_size);

#ifdef DEBUG_NNDSP
    print_float_vector("x_out", x_out, frame_size);
#endif

    /* buffer update */
    OPUS_COPY(hAdaComb->last_kernel, kernel_buffer, kernel_size);
    OPUS_COPY(hAdaComb->history, p_input + frame_size - kernel_size - ADACOMB_MAX_LAG, kernel_size + ADACOMB_MAX_LAG);
    hAdaComb->last_pitch_lag = pitch_lag;
    hAdaComb->last_global_gain = global_gain;
}
