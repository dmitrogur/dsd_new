#ifndef DSD_FIR_H
#define DSD_FIR_H

#include <stddef.h>

// Эта конструкция будет активна, только если файл включается в C++ код.
// C компилятор ее проигнорирует.
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Структура, описывающая один экземпляр КИХ-фильтра.
 */
typedef struct {
    const float* coeffs;
    float* state;
    size_t num_taps;
    size_t state_index;
    float gain;
} dsd_fir_filter_t;

/**
 * @brief Инициализирует экземпляр КИХ-фильтра.
 * @return 0 в случае успеха, -1 в случае ошибки.
 */
int dsd_fir_init(dsd_fir_filter_t* filter, const float* coeffs, size_t num_taps, float gain);

/**
 * @brief Освобождает память, выделенную для состояния фильтра.
 */
void dsd_fir_destroy(dsd_fir_filter_t* filter);

/**
 * @brief Обрабатывает один сэмпл, используя кольцевой буфер.
 * @return Отфильтрованный сэмпл.
 */
short dsd_fir_process(dsd_fir_filter_t* filter, short input_sample);

/**
 * @brief Сбрасывает состояние фильтра (заполняет буфер нулями).
 */
void dsd_fir_reset(dsd_fir_filter_t* filter);


#ifdef __cplusplus
}
#endif

#endif // DSD_FIR_H