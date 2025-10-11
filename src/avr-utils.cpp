
#include "avr-utils.h"

namespace avr {

// ----------------------

#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>

using namespace std;

static vector<uint8_t> createWavHeader(const vector<int16_t>& pcmData) {
    const uint32_t sampleRate = 8000;
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;

    uint32_t dataSize = pcmData.size() * sizeof(int16_t);
    uint32_t chunkSize = 36 + dataSize;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;

    vector<uint8_t> header(44, 0);

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = chunkSize & 0xFF;
    header[5] = (chunkSize >> 8) & 0xFF;
    header[6] = (chunkSize >> 16) & 0xFF;
    header[7] = (chunkSize >> 24) & 0xFF;
    header[8]  = 'W'; header[9]  = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;
    header[22] = numChannels & 0xFF;
    header[23] = (numChannels >> 8) & 0xFF;
    header[24] = sampleRate & 0xFF;
    header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF;
    header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = blockAlign & 0xFF;
    header[33] = (blockAlign >> 8) & 0xFF;
    header[34] = bitsPerSample & 0xFF;
    header[35] = (bitsPerSample >> 8) & 0xFF;
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = dataSize & 0xFF;
    header[41] = (dataSize >> 8) & 0xFF;
    header[42] = (dataSize >> 16) & 0xFF;
    header[43] = (dataSize >> 24) & 0xFF;

    return header;
}

void save_wav(const string &filename, const vector<int16_t> &pcm) {
    ofstream out(filename, ios::binary);
    if (!out) {
        cerr << "Ошибка при открытии файла: " << filename << endl;
        return;
    }

    vector<uint8_t> header = createWavHeader(pcm);
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(int16_t));
    out.close();
}

// ----------------------

vector<int16_t> read_pcm(const string& filename) {
    vector<int16_t> data;
    ifstream file(filename, std::ios::binary | std::ios::ate);

    if (!file) {
//        std::cerr << "Ошибка при открытии файла: " << filename << std::endl;
        return data;
    }

    std::streamsize size = file.tellg();
    if (size % sizeof(int16_t) != 0) {
//        std::cerr << "Размер файла не кратен размеру int16_t, возможно, повреждён." << std::endl;
        return data;
    }

    file.seekg(0, std::ios::beg);
    data.resize(size / sizeof(int16_t));

    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
//        std::cerr << "Ошибка при чтении файла: " << filename << std::endl;
        data.clear();
    }

    return data;
}

void add_set(set<string> &dest, const set<string> &src)
{
    for(const auto &x: src)
	dest.insert(x);
};

string sset_to_str(const set<string> &k)
{
    bool first = false;
    string res;
    
    for(const auto &s: k) {
	if(first)
	    res += ", ";
	first = true;
	res += s;
    };
    
    return res;
};


// ----------------------
// ----------------------


}; // avr
