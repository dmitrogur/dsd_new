
#include <stdlib.h>
#include <string.h>
#include "dsd_fir.h" // Включаем наш собственный заголовочный файл

// Эта конструкция будет активна, только если файл включается в C++ код.
// C компилятор ее проигнорирует.

int dsd_fir_init(dsd_fir_filter_t* filter, const float* coeffs, size_t num_taps, float gain) {
    if (filter == NULL || coeffs == NULL || num_taps == 0) return -1;
    filter->coeffs = coeffs;
    filter->num_taps = num_taps;
    filter->gain = gain;
    filter->state = NULL;
    filter->state_index = 0;
    filter->state = (float*)malloc(num_taps * sizeof(float));
    if (filter->state == NULL) return -1;
    dsd_fir_reset(filter);
    return 0;
}

void dsd_fir_destroy(dsd_fir_filter_t* filter) {
    if (filter != NULL && filter->state != NULL) {
        free(filter->state);
        filter->state = NULL;
    }
}

void dsd_fir_reset(dsd_fir_filter_t* filter) {
    if (filter != NULL && filter->state != NULL) {
        memset(filter->state, 0, filter->num_taps * sizeof(float));
        filter->state_index = 0;
    }
}

#include <xmmintrin.h> // SSE

short dsd_fir_process(dsd_fir_filter_t* filter, short input_sample) {
    float sum;
    size_t i;
    size_t num_taps = filter->num_taps;
    
    // 1. Обновляем индекс кольцевого буфера
    // state_index указывает на самую старую позицию, которую мы сейчас перезапишем
    if (filter->state_index == 0) {
        filter->state_index = num_taps - 1;
    } else {
        filter->state_index--;
    }
    filter->state[filter->state_index] = (float)input_sample;

    // 2. Выпрямляем кольцевой буфер во временный массив на стеке
    // Это компромисс между кольцевым буфером и требованиями SIMD
    float ordered_state[num_taps]; // VLA, требует C99. Если компилятор старый, нужно использовать alloca или malloc.
    
    size_t head_len = num_taps - filter->state_index;
    memcpy(ordered_state, &filter->state[filter->state_index], head_len * sizeof(float));
    if (filter->state_index > 0) {
        memcpy(&ordered_state[head_len], filter->state, filter->state_index * sizeof(float));
    }

    // 3. Выполняем быструю свертку с помощью SIMD
    size_t num_blocks_sse = num_taps / 4;
    __m128 sum_vec = _mm_setzero_ps();

    for (i = 0; i < num_blocks_sse; ++i) {
        __m128 coeffs_vec = _mm_loadu_ps(filter->coeffs + i * 4);
        __m128 state_vec = _mm_loadu_ps(ordered_state + i * 4);
        sum_vec = _mm_add_ps(sum_vec, _mm_mul_ps(coeffs_vec, state_vec));
    }
    
    // Горизонтальное сложение
    float temp_sum[4];
    _mm_storeu_ps(temp_sum, sum_vec);
    sum = temp_sum[0] + temp_sum[1] + temp_sum[2] + temp_sum[3];
    
    // Обрабатываем "хвост"
    for (i = num_blocks_sse * 4; i < num_taps; ++i) {
        sum += filter->coeffs[i] * ordered_state[i];
    }
    // --- ЗАЩИТА ---
    if (filter->gain == 0.0f) {
        // Это нештатная ситуация, но лучше вернуть 0, чем упасть
        return 0; 
    }
        
    return (short)(sum / filter->gain);
}
/*
short dsd_fir_process(dsd_fir_filter_t* filter, short input_sample) {
    float sum = 0.0f;
    size_t i;
    size_t state_buf_idx;

    // Вставляем новый сэмпл в кольцевой буфер
    filter->state[filter->state_index] = (float)input_sample;

    // Сдвигаем индекс. Если дошли до начала, перескакиваем в конец.
    if (filter->state_index == 0) {
        filter->state_index = filter->num_taps - 1;
    } else {
        filter->state_index--;
    }
    
    // Вычисляем свертку
    state_buf_idx = filter->state_index;
    for (i = 0; i < filter->num_taps; i++) {
        if (state_buf_idx >= filter->num_taps - 1) {
            state_buf_idx = 0;
        } else {
            state_buf_idx++;
        }
        sum += filter->coeffs[i] * filter->state[state_buf_idx];
    }
    return (short)(sum / filter->gain);
} */