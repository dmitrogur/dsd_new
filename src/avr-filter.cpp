#include <iostream>
#include <cstring>
#include <vector>
#include <cmath>
#include <deque>
#include <iomanip>
#include <algorithm>  // Добавляем для std::max_element()
// #include <CL/opencl.hpp>  // OpenCL

int use_avr_filter = 0;

namespace avr_filter {

class RRCFilter;


// Параметры RRC-фильтра
const int num_taps = 201;   // Увеличенная длина фильтра
const double symbol_rate = 6650.0;  // Символьная скорость (S/s)
const double sampling_rate = 48000.0;  // Частота дискретизации (Гц)
const double roll_off = 0.3;  // Оптимизированный roll-off

static std::vector<double> rrc_filter;
static RRCFilter *filter = 0;


// Функция для вычисления окна Чебышева
std::vector<double> chebyshev_window(int size, double attenuation) {
    std::vector<double> window(size);
    double tg = pow(10.0, attenuation / 20.0);

    for (int i = 0; i < size; i++) {
        double sum = 0.0;
        for (int k = 1; k <= (size - 1) / 2; k++) {
            sum += cos(k * M_PI * (i - (size - 1) / 2.0) / (size - 1)) * cosh(k * acosh(tg) / (size - 1));
        }
        window[i] = tg + 2 * sum;
    }

    // Нормализация
    double max_val = *std::max_element(window.begin(), window.end());
    for (auto &w : window) {
        w /= max_val;
    }

    return window;
}

// Функция для генерации RRC-фильтра
std::vector<double> generate_rrc_filter() {
    std::vector<double> rrc_filter(num_taps);
    double T = 1.0 / symbol_rate;
    double Ts = 1.0 / sampling_rate;
    int center = num_taps / 2;

    for (int i = 0; i < num_taps; i++) {
        double t = (i - center) * Ts;

        if (t == 0.0) {
            rrc_filter[i] = 1.0 + roll_off * (4.0 / M_PI - 1.0);
        } else if (std::fabs(4.0 * roll_off * t / T) == 1.0) {
            rrc_filter[i] = (roll_off / sqrt(2)) * 
                            ((1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * roll_off)) + 
                             (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * roll_off)));
        } else {
            rrc_filter[i] = (sin(M_PI * t / T * (1 - roll_off)) + 
                             4 * roll_off * t / T * cos(M_PI * t / T * (1 + roll_off))) /
                            (M_PI * t / T * (1 - (4 * roll_off * t / T) * (4 * roll_off * t / T)));
        }
    }

    // Применяем окно Чебышева (затухание 60 дБ)
    std::vector<double> cheby_win = chebyshev_window(num_taps, 60.0);
    for (int i = 0; i < num_taps; i++) {
        rrc_filter[i] *= cheby_win[i];
    }

    // Нормализация
    double sum = 0.0;
    for (double coef : rrc_filter) sum += coef;
    for (double &coef : rrc_filter) coef /= sum;

    return rrc_filter;
}

// Функция для скользящего среднего
std::vector<double> moving_average_filter(const std::vector<double>& signal) {
    std::vector<double> output(signal.size());
    for (size_t i = 1; i < signal.size() - 1; i++) {
        output[i] = (signal[i-1] + signal[i] + signal[i+1]) / 3.0;
    }
    return output;
}

#if 1
// Класс RRC-фильтра для реального времени
class RRCFilter {
private:
    std::vector<double> coeffs;
    std::deque<double> buffer;  // FIFO-буфер последних отсчётов

public:
    RRCFilter(const std::vector<double>& filter_coeffs) : coeffs(filter_coeffs) {
        buffer.resize(coeffs.size(), 0.0);  // Заполняем буфер нулями
    }

    // Фильтрация одного сэмпла
    double process(double sample) {
        buffer.pop_front();
        buffer.push_back(sample);

        double result = 0.0;
        for (size_t i = 0; i < coeffs.size(); i++) {
            result += buffer[i] * coeffs[i];
        }

        return result;
    }
};
#else
const char* kernel_code = R"CLC(
__kernel void rrc_filter_gpu(
    __global const float* input,
    __global const float* filter,
    __global float* output,
    const int filter_size
) {
    int gid = get_global_id(0);
    float sum = 0.0;

    for (int i = 0; i < filter_size; i++) {
        sum += input[gid + i] * filter[i];
    }
    output[gid] = sum;
}
)CLC";

// Класс RRC-фильтра с OpenCL
class RRCFilter {
private:
    std::vector<double> coeffs;
    std::deque<double> buffer;
    bool use_gpu;
    cl::Context context;
    cl::CommandQueue queue;
    cl::Program program;
    cl::Kernel kernel;
    cl::Buffer buf_input, buf_filter, buf_output;

public:
    RRCFilter(const std::vector<double>& filter_coeffs) : coeffs(filter_coeffs), use_gpu(false) {
        buffer.resize(coeffs.size(), 0.0);  

try {
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    if (platforms.empty()) throw std::runtime_error("No OpenCL platforms found.");

    cl::Platform platform = platforms.front();
    std::vector<cl::Device> devices;
    platform.getDevices(CL_DEVICE_TYPE_GPU, &devices);
    if (devices.empty()) throw std::runtime_error("No OpenCL GPU devices found.");

    cl::Device device = devices.front();
    context = cl::Context(device);
    queue = cl::CommandQueue(context, device);

    // Исправленный код:
    std::string kernel_string(kernel_code);
    cl::Program::Sources sources;
    sources.push_back(kernel_string);  // Теперь это корректно!

    program = cl::Program(context, sources);
    program.build({device});
    kernel = cl::Kernel(program, "rrc_filter_gpu");

    buf_filter = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float) * coeffs.size(), coeffs.data());
    use_gpu = true;
    std::cout << "OpenCL GPU фильтрация включена." << std::endl;
} catch (const std::exception& e) {
    std::cerr << "OpenCL отключен: " << e.what() << std::endl;
}
    }

    // Фильтрация одного сэмпла (GPU или CPU)
    double process(double sample) {
        if (!use_gpu) {
            // CPU-реализация
            buffer.pop_front();
            buffer.push_back(sample);
            double result = 0.0;
            for (size_t i = 0; i < coeffs.size(); i++) {
                result += buffer[i] * coeffs[i];
            }
            return result;
        } else {
            // GPU-реализация
            std::vector<float> input_data(coeffs.size());
            for (size_t i = 0; i < coeffs.size(); i++) {
                input_data[i] = (float)buffer[i];
            }
            buffer.pop_front();
            buffer.push_back(sample);

            buf_input = cl::Buffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float) * coeffs.size(), input_data.data());
            buf_output = cl::Buffer(context, CL_MEM_WRITE_ONLY, sizeof(float));

            kernel.setArg(0, buf_input);
            kernel.setArg(1, buf_filter);
            kernel.setArg(2, buf_output);
            kernel.setArg(3, (int)coeffs.size());

            queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(1), cl::NullRange);
            queue.finish();

            float result;
            queue.enqueueReadBuffer(buf_output, CL_TRUE, 0, sizeof(float), &result);
            return result;
        }
    }
};
#endif


static int xmain() {
    // Генерация RRC-фильтра
    std::vector<double> rrc_filter = generate_rrc_filter();
    RRCFilter filter(rrc_filter);

    // Генерация тестового сигнала (имитация DMR-сигнала)
    int signal_length = 100;
    std::vector<double> dmr_signal(signal_length);
    for (int i = 0; i < signal_length; i++) {
        dmr_signal[i] = sin(2 * M_PI * 1200 * i / sampling_rate) + 0.3 * ((rand() % 100) / 100.0);
    }

    // Применение скользящего среднего перед фильтрацией
    std::vector<double> smoothed_signal = moving_average_filter(dmr_signal);

    // Фильтрация сигнала в реальном времени (по сэмплу)
    std::cout << "Filtered Signal (first 10 samples):\n";
    for (int i = 0; i < 10; i++) {
        double filtered_sample = filter.process(smoothed_signal[i]);
        std::cout << std::fixed << std::setprecision(6) << filtered_sample << " ";
    }
    std::cout << std::endl;

    return 0;
};

static void rrc_init(void)
{
    rrc_filter = generate_rrc_filter();
    filter = new RRCFilter(rrc_filter);
    
    const char *s_af = getenv("AVR_DSD_USE_FILTER");
    if(s_af && !strcmp(s_af, "1"))
	use_avr_filter = 1;
    
    use_avr_filter = 0;

};

class RRC_Init {
public:
    RRC_Init()
    {
	rrc_init();
	
    };
};

static RRC_Init _rrc_init;

}; // avr_filter

extern "C" short avr_dmr_input_filter(short sample)
{
    return   avr_filter::filter->process(sample);
};

