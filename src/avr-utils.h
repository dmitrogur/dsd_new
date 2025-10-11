#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <fstream>
#include <iostream>
#include <sstream>

namespace avr {

using namespace std;

// ----------------------

void save_wav(const string &filename, const vector<int16_t> &pcm);

vector<int16_t> read_pcm(const string& filename);

// ----------------------

void add_set(set<string> &dest, const set<string> &src);
string sset_to_str(const set<string> &k);

// ----------------------
// ----------------------

}; // avr