#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
        
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <fcntl.h>
    
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <thread>
#include <chrono>

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <set>

#include "avr-utils.h"

extern "C" {
extern unsigned int dmr_filter_sample_num;
};

namespace avr {

using namespace std;

static unsigned long signal_start_ut = 0;

static ofstream *flog = 0;
static string *wdir = 0;

static bool dmr_check = false;
static int dmr_check_count = 0;

static string dlg_tmp_prefix;

unsigned int last_sample_num = 0;

static string pcm_index_file;

// ----------------------

/// Записывает содержимое `data` в бинарный файл `file_name`.
/// Бросает std::ios_base::failure, если файл открыть/записать не удалось.
void save_pcm_index(const std::vector<uint32_t>& data,
                  const std::string&          file_name)
{
    // Открываем файл в режиме «запись-только» + бинарный
    std::ofstream out(file_name, std::ios::binary);
    if (!out)
        throw std::ios_base::failure("Невозможно открыть файл " + file_name);

    // Сохраняем данные «как есть»
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(uint32_t)));

    if (!out)  // проверяем, что запись прошла без ошибок
        throw std::ios_base::failure("Ошибка записи в файл " + file_name);
}



// ----------------------

static vector<int16_t> audio_slot1, audio_slot2, beep;
static vector<uint32_t> audio_index;

void add_slot_audio(const void *data, unsigned sz)
{
    unsigned samples = sz / 4;
    const auto *p = (int16_t*)data;
    const auto *e = p + (samples*2);
    
    while(p < e) {
	audio_slot1.push_back(p[0]);
	audio_slot2.push_back(p[1]);
	audio_index.push_back(last_sample_num);
	p += 2;
    };
    
};

struct AudioFragment {
    const int16_t *data = 0;
    unsigned samples = 0;;
    
    void clear(void) { data=0; samples = 0; };
};

AudioFragment audio_find_fragment(unsigned as_start, unsigned as_end, int slot)
{
    const unsigned dt_ms = 48000 / 1000;
    const unsigned dt = dt_ms * 20 * 3;
    AudioFragment f;
    if(as_start >= as_end)
	return f;

    if(as_start > dt)
	as_start -= dt;
    else
	as_start = 1;

    as_end += dt;

    const auto &a = (slot == 1) ? audio_slot1 : audio_slot2;
    const unsigned *p = audio_index.data();
    unsigned pe = audio_index.size();
        
    unsigned start_index = 0;
    for(; start_index < pe; ++start_index)
	if(p[start_index] >= as_start)
	    break;
	
    if(start_index >= pe)
	return f;

    unsigned end_index = start_index + 1;
    for(unsigned ei=end_index; end_index < pe; ++ei) {
	if(p[ei] <= as_end)
	    end_index = ei;
	else
	break;
    };

    if(start_index < end_index) {
	f.data = a.data() + start_index;
	f.samples = end_index - start_index;
    };

    return f;
};


// ----------------------
//void log_printf(int level, const char *fmt_spec, ...) __attribute__ ((format (printf, 2, 3)));

void log_printf(const char *fmt_spec, ...) __attribute__ ((format (printf, 1, 2)));
void log_flush(void) { if(flog) flog->flush(); };

#define LOG log_printf
#define LOGF log_flush()

void log_printf(const char *fmt_spec, ...)
{
    timeval st;
    struct timezone tz;

    if(!flog || !*flog)
	return;

    if(!fmt_spec || !fmt_spec[0])
    {
	*flog << "\n";
//        fprintf(log_file, "\n");
//        fflush(log_file);
        return;
    };  

    gettimeofday(&st, &tz);
        
    time_t tu = st.tv_sec;
    tm t;
    localtime_r(&tu, &t);

    char buf[16000];
    sprintf(buf, "%02d.%02d.%04d-%02d:%02d:%02d.%06d: ", t.tm_mday,
        t.tm_mon+1, t.tm_year+1900, t.tm_hour, t.tm_min, t.tm_sec, (int)st.tv_usec);
    *flog << buf;
    
    va_list arglist;
    va_start (arglist, fmt_spec);
    vsprintf(buf, fmt_spec, arglist);
    *flog << buf;
    va_end(arglist);

    *flog << "\n";

//    fprintf(log_file, "\n");
    
//    fflush(log_file);
};  




// ----------------------

static void trimInPlace(std::string& str) {
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));

    str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), str.end());
};

std::string to_uppercase(const std::string& input) {
    std::string result = input;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
};

static string to_hex(unsigned x)
{
    char buf[64];
    buf[0] = 0;
    if(x)
	sprintf(buf, "0x%X", x);
    return string(buf);
};

// ----------------------
static string *empty_string;

using Dict = map<string, string>;

struct Record {
    Dict m;
    
    int as = -1;
    int slot = -1;
    int err = 0;
    
    string info;
    
    Record() {};
    Record(const Dict &_m) : m(_m) {};
    
    void init(void);
    
    const string& Str(const string &key) const;
    int Int(const string &key, int def=0) const;
    unsigned UInt(const string &key, unsigned def=0) const;
    
};

void Record::init(void)
{
    as = Int("as", -1);
    slot = Int("slot", -1);
    err = Int("err", 0);

    info = "as="; info += to_string(as);
    info += " err="; info += to_string(err);

    {
	string b_type = Str("b_type");
	if(!b_type.empty())
	    m["b_type"] = to_uppercase(b_type);
	info += " b_type="; info += b_type;
    };
    
    if(slot == -1) {
	if(Str("b_type") == "MS")
	    slot = 1;
    };
    info += " slot="; info += to_string(slot);
    
    if(!dmr_check)
	return;
    
    if(err)
	return;
    
    const string &vp = Str("vp");
    if(vp == "IDLE" || vp == "VLC" || vp == "TLC" || vp == "CSBK")
	dmr_check_count++;
    else {
	const string &vc = Str("vc");
	if(!vc.empty())
	    dmr_check_count++;
    };

    if(dmr_check_count < 5)
	return;

    string name = *wdir + "/dmr-found";
    ::unlink(name.c_str());
    {
	ofstream f(name, ios::out);
	f << "dmr_found=1\n";
    };
    exit(0);
};

const string& Record::Str(const string &key) const
{
    auto j = m.find(key);
    return (j != m.end()) ? j->second : *empty_string;
};

int Record::Int(const string &key, int def) const
{
    auto j = m.find(key);
    if(j != m.end()) {
	try {
	    return std::stoi(j->second);
	} catch(...) {};
    };
    
    return def;
};

unsigned Record::UInt(const string &key, unsigned def) const
{
    auto j = m.find(key);
    if(j != m.end()) {
	try {
	    return std::stoul(j->second);
	} catch(...) {};
    };
    
    return def;
};

// ----------------------

using PRecords = vector<shared_ptr<Record>>;
PRecords *s1 = 0;
PRecords *s2 = 0;


// ----------------------

struct Log {
    ofstream *f = 0;

    Dict m;

    Log(const string &log_name);
    ~Log();
    
    void line_flush(void);
    
    void new_line(const string &type);
    
    void add(const string &key, const string &value);
    void adds(const string &key, const string &value);
    void add(const string &key, int value);
    void add(const string &key, unsigned value);
};

Log::Log(const string &log_name)
{
    ::unlink(log_name.c_str());
    f = new ofstream(log_name, ios::out);
};

Log::~Log()
{
    line_flush();
    delete f;
};
    
void Log::line_flush(void)
{
    if(!m.size() || !f)
	return;


    string s;
    for(const auto &x: m)
	s += x.first + "=''" + x.second + "'' ";

    *f << s << endl;
    
    auto pm = make_shared<Record>(m);
    pm->init();
    
    int no_sync = pm->Int("no_sync");
    if(no_sync) {
//	LOG("AR: NO SYNC: %s", pm->info.c_str());
	const auto &frame = pm->Str("frame");
	bool by_frame = (!frame.empty() && (frame.find("DMR") != string::npos));

	const auto &att = pm->Str("att");
	bool by_att = (att=="sync-bs" );
	bool by_att1 = (att=="sync-ms-data" || att=="sync-ms");
	
	
	
//	LOG("AR: NO SYNC_: by_frame=%d by_att=%d by_att1=%d", (int)by_frame, (int)by_att, (int)by_att1);
	
	if( by_frame || by_att || by_att1) {
	    s1->push_back(pm);
	    
	    if(!by_att1)
		s2->push_back(pm);
	};
    }
    else {
//	LOG("AR: %s", pm->info.c_str());
	if( (pm->as >= 0) && (pm->slot >= 0) ) {
	    if(pm->slot == 1)
		s1->push_back(pm);
	    else
	    if(pm->slot == 2)
		s2->push_back(pm);
	};
    };
    
    m.clear();
};


void Log::new_line(const string &type)
{
    line_flush();

    m["as"] = to_string(dmr_filter_sample_num);

    double t = (double)dmr_filter_sample_num / 48000.0;
    char buf[32];
    sprintf(buf, "%.3f", t);
    m["at"] = buf;
    m["att"] = type;
};

void Log::add(const string &key, const string &value)
{
    string v = value;
    trimInPlace(v);
    m[key] = v;
};

void Log::adds(const string &key, const string &value)
{
    string v = value;
    trimInPlace(v);
    if(v.empty())
	return;

    string z = m[key];
    if(!z.empty())
	z += "|";
    z += v;
    m[key] = z;
};



void Log::add(const string &key, int value)
{
    m[key] = to_string(value);
};

void Log::add(const string &key, unsigned value)
{
    m[key] = to_string(value);
};

// ----------------------

struct Session {
    int as_start = 0;
    int as_end = 0;
    int slot = 0;
    int cc = 0;
    
    int tgt = 0;
    int src = 0;

    string call_type;
    int encrypted = 0;
    int alg_id = 0;
    int key_id = 0;
    int mi = 0;
    
    string type;
    
    string mode;

    int id_com = 0;
    int id_dialog = 0;

    set<string> ext_info;


    bool use = false;
    
    uint64_t hid = 0;
    
    void clear(void);
    
    string to_string(int g_offs=6, int g_tab=3) const;
    
    void make_hid(void)
    {
	int a = tgt;
	int b = src;
	if(a < b)
	    swap(a, b);
	hid = ((uint64_t)a << 32) | (uint64_t)b;
	if(hid == 0) {
	    if(mode=="VLC" || mode=="TLC" || mode=="DATA")
		hid = ~((uint64_t)0);
	};
    };

    bool is_active(void) const { return (mode=="VLC" || mode=="TLC" || mode=="DATA"); };    
    uint64_t mhid(void) const { return ((uint64_t)tgt << 32) | (uint64_t)src; };

    void add_ei(const string &s);
};

void Session::clear(void) {
    as_start = as_end = slot = cc = 0;
    tgt = src = 0;
    call_type.erase();
    encrypted = alg_id = key_id = mi = 0;
    use = false;
    mode.erase();
    type.erase();
    id_com = 0;
    id_dialog = 0;
    hid = 0;
};


void Session::add_ei(const string &s)
{
    if(!s.empty())
	ext_info.insert(s);
};


string Session::to_string(int g_offs, int g_tab) const
{
    char buf[64];
    stringstream f;
    string offs_s;
    for(int i=0; i<g_offs; ++i)
	offs_s += ' ';
	
    f << offs_s << "{\n";
    for(int i=0; i<g_tab; ++i)
	offs_s += ' ';


    f << offs_s << "\"mode\": \"" <<  mode << "\",\n";
    f << offs_s << "\"start\": " << as_start << ", ";
    f << "\"end\": " << as_end << ", ";

    double start_sec = (double)(as_start) / 48000.0;
    sprintf(buf, "%.3f", start_sec);
    f << "\"start_sec\": " << string(buf) << ", ";
    
    unsigned long start_ut = start_sec + signal_start_ut;
    f << "\"start_ut\": " << start_ut << ", ";

    double duration = (double)(as_end - as_start) / 48000.0;
    sprintf(buf, "%.3f", duration);
    f << "\"duration\": " << string(buf) << ",\n";
    
    f << offs_s << "\"type\": \"" << type << "\", ";
    f << "\"inversion\": 0, ";
    f << "\"slot\": " << slot << ", ";
    f << "\"color\": " << cc << ",\n";
    
    f << offs_s << "\"tgt\": " << tgt << ", ";
    f << "\"src\": " << src << ", ";
    f << "\"id_com\": " << id_com << ", ";
    f << "\"id_dialog\": " << id_dialog << ",\n";
    
    f << offs_s << "\"call_type\": \"" << call_type << "\", ";
    f << "\"encrypted\": " << encrypted << ", ";
    f << "\"alg_id\": \"" << to_hex(alg_id) << "\", ";
    f << "\"key_id\": \"" << to_hex(key_id) << "\", ";
    f << "\"mi\": \"" << to_hex(mi) << "\"\n";
    
    for(int i=0; i<g_offs; ++i)
	f << ' ';
    f << '}';
    return f.str();
};



static vector<Session> *ss;

// ----------------------

static set<string> modes = { "VLC", "TLC", "IDLE", "DATA", "CSBK" };

int session_interval = 72000;

const int ST_IDLE = 2 * 48000;

static int global_id_com = 0;
static void detect_sessions(const PRecords &v, int slot)
{
    int g_id_com = 0; //global_id_com;
    
    Session lk;
    Session k;
    const Record *last = 0;;

//    if(flog && *flog) *flog << "Start: " << slot << "\n";

    for(const auto &z: v) {
	
	string mode;
	
	int no_sync = z->Int("no_sync", 0);
	if(no_sync) {
	    const auto &frame = z->Str("frame");
	    bool by_frame = (!frame.empty() && (frame.find("DMR") != string::npos));
	    
	    const auto &att = z->Str("att");
	    bool by_att = (att=="sync-bs" || att=="sync-ms");
	    
	    if(by_frame || by_att) {
		if(k.use) {
		    if(!k.mode.empty()) {
			
			if(k.is_active()) {
			    if(lk.is_active() && (lk.mhid() != k.mhid())) { ++g_id_com; k.id_com = g_id_com; };
			    lk = k;
			    //if(flog && *flog) *flog << "k: " << k.is_active() << " " << k.tgt << " " << k.src << " " << k.mhid() << endl;
			};
	    	        ss->push_back(k);
		    };
		    k.clear();
		};
		++g_id_com;
		last = z.get();
		continue;
	    };
	};
	
	if(z->err) {
	    last = z.get();
	    //if(flog && *flog) *flog << z->as << ": " << "err\n";
	    continue;
	};
	
	const string &vp = z->Str("vp");
	string vc = vp;
	mode = vp;
	if(vc.empty()) {
	    vc = z->Str("vc");
	    if(!vc.empty())
		mode = "VLC";
	};
	    
	if(mode.empty()) {
	    last = z.get();
	    //if(flog && *flog) *flog << z->as << ": " << "mode.empty\n";
	    continue;
	};


	if(last && (z->as - last->as) > session_interval) {
		++g_id_com;

	    if(!k.mode.empty()) {
		if(k.is_active()) {
		    if(lk.is_active() && (lk.mhid() != k.mhid())) { ++g_id_com; k.id_com = g_id_com; };
		    lk = k;
		    //if(flog && *flog) *flog << "k: " << k.is_active() << " " << k.tgt << " " << k.src << " " << k.mhid()<< endl;
		};
		ss->push_back(k);
	    };
	    k.clear();
	    
	    //if(flog && *flog) *flog << z->as << ": " << "clear1\n";
	};

	last = z.get();;

	bool x_mode = false;

	auto j = modes.find(mode);
	if(j != modes.end()) {
	    x_mode = true;
	    if(k.use && (mode != k.mode)) {
		if((z->as - k.as_end) < session_interval)
		    k.as_end = z->as;
		if(!k.mode.empty()) {
		    if(k.is_active()) {
			if(lk.is_active() && (lk.mhid() != k.mhid())) { ++g_id_com; k.id_com = g_id_com; };
		        lk = k;
			//if(flog && *flog) *flog << "k: " << k.is_active() << " " << k.tgt << " " << k.src << " " << k.mhid()<< endl;
		    };
		    ss->push_back(k);
		};
		//if(flog && *flog) *flog << z->as << ": " << "clear2(" << slot << "): " << mode << " | " << k.mode << "\n";
		k.clear();
	    };
	};


	if(!k.use) {
	    if(!x_mode) {
	    //if(flog && *flog) *flog << z->as << ": " << "K1\n";
		continue;
	    };
		
	    k.as_start = k.as_end = z->as;
	    k.use = true;
	    k.slot = slot;
	    k.mode = mode;
	    
	    if(!g_id_com)
		g_id_com = 1;
	    k.id_com = g_id_com;
	};

	k.as_end = z->as;
	
	int cc = z->Int("tcc");
	if(cc) k.cc = cc;
	
	int src = z->Int("kSRC");
	if(src) k.src = src;

	int tgt = z->Int("kTGT");
	if(tgt) k.tgt = tgt;
	
	string type = z->Str("b_type");
	if(!type.empty()) k.type = type;

	string ct = z->Str("kCall");
	if(!ct.empty()) k.call_type = ct;

	int enc = z->Int("kEncrypted");
	if(enc) k.encrypted = enc;

	int alg_id = z->Int("kALG_ID");
	if(alg_id) k.alg_id = alg_id;

	int key_id = z->Int("kKEY_ID");
	if(key_id) k.key_id = key_id;

	int mi = z->Int("kPI_MI");
	if(mi) k.mi = mi;
	
	k.add_ei(z->Str("flco_info"));
    };
    
    if(k.use)
	if(!k.mode.empty())
	    ss->push_back(k);
    
    global_id_com = g_id_com;
    
    std::sort(ss->begin(), ss->end(),
              [](const Session& a, const Session& b) {
                  return a.as_start < b.as_start;
              });

    map<int, int> mcom;
    for(const auto &k: *ss)
	mcom[k.id_com] = 0;

    int mcom_num = 0;
    for(auto &x: mcom)
	x.second = ++mcom_num;

    for(auto &k: *ss)
	k.id_com = mcom[k.id_com];



    const int ST = 120 * 48000;
    
    struct SessionCursor {
	uint64_t hid = 0;
	int last_end = 0;
	int id_com = 0;
	
	SessionCursor() = default;
	SessionCursor(uint64_t _hid, int _id_com=0) : hid(_hid), id_com(_id_com) { };
    };
    
    map<uint64_t, SessionCursor> sc;
    g_id_com = 0;
    
    for(auto &k: *ss) {
	k.make_hid();
	if(k.hid)
	    sc[k.hid] = SessionCursor(k.hid);
    };

    set<string> smodes = { "VLC", "TLC", "DATA" };

    for(auto &k: *ss) {
	if(!k.hid)
	    continue;
	    
	if(smodes.find(k.mode) == smodes.end())
	    continue;
	
	auto j = sc.find(k.hid);
	if(j == sc.end())
	    continue;
	
	auto &c = j->second;
	
	if(!c.id_com)
	    c.id_com = ++g_id_com;
	
	if(c.last_end) {
	    int st = k.as_start - c.last_end;
	    if(st >= ST)
		c.id_com = ++g_id_com;
	};
	
	k.id_dialog = c.id_com;
	c.last_end = k.as_end;
	
    };
    
    
//    int id_com = 0;
//    for(auto &x: *ss)
//	x.id_com = ++id_com;
    
    int vlc1 = 0;
    int vlc1_enc = 0;

    int vlc2 = 0;
    int vlc2_enc = 0;
    
    for(const auto &x: *ss) {
	if(x.mode != "VLC")
	    continue;
	if(slot == 1)
	    vlc1++;
	if(slot == 2)
	    vlc2++;
	
	if(x.encrypted) {
	    if(slot == 1)
		vlc1_enc++;
	    if(slot == 2)
		vlc2_enc++;
	};
    };
    
    int vlc1_voice = (vlc1 && !vlc1_enc);
    int vlc2_voice = (vlc2 && !vlc2_enc);
    
    string snm = *wdir + "/voice.stat";
    ::unlink(snm.c_str());
    ofstream f(snm, ios::out);
    f << "vs_vlc1=" << vlc1 << endl;
    f << "vs_vlc1_enc=" << vlc1_enc << endl;
    f << "vs_vlc1_voice=" << vlc1_voice << endl;
    f << "vs_vlc2=" << vlc2 << endl;
    f << "vs_vlc2_enc=" << vlc2_enc << endl;
    f << "vs_vlc2_voice=" << vlc2_voice << endl;
};

// ----------------------
static void detect(void)
{
    detect_sessions(*s1, 1);
    detect_sessions(*s2, 2);
};

// ----------------------

struct Dialog {
    vector<Session*> sq;
    
    int id_dialog = 0;
    
    int as_start = 0;
    int as_end = 0;
    int tgt=0;
    int src = 0;
    
    int slot = 0;
    int cc = 0;
    
    int alg_id = 0;
    int key_id = 0;
    
    string type;
    string mode;
    
    vector<int16_t> audio;

    set<string> call_type;
    vector<string> session;

    set<string> ext_info;

    void add_audio(const int16_t *data, unsigned samples);
    void add_audio(const vector<int16_t> &data);

    void save_audio(const string &fnm);
};

void Dialog::add_audio(const int16_t *data, unsigned samples)
{
    if(!data || !samples)
	return;

    unsigned last = audio.size();
    audio.resize(last + samples);
    memcpy(audio.data() + last, data, samples*2);
};

void Dialog::add_audio(const vector<int16_t> &data)
{
    add_audio(data.data(), data.size());
};



void Dialog::save_audio(const string &fnm)
{
    if(audio.size())
	save_wav(fnm, audio);
};


// ----------------------

static void save_dialogs(ofstream &f)
{
    LOG("Dialogs");
    set<int> s_did;
    for(const auto &x: *ss) {
//	if(x.tgt && x.src)
	    s_did.insert(x.id_dialog);
    };

    map<int, Dialog> d;
    
    for(auto id_dialog: s_did) {
	LOG("DG start: %d", id_dialog);
	for(auto &m: *ss) {
	    if(m.id_dialog != id_dialog)
		continue;
	
	    if(!(m.mode=="VLC" || m.mode=="TLC"))
		continue;
	
	    if(m.as_start == m.as_end) {
		LOG("DG[%d]: m.as_start==m.as_end (%d) - ignore", id_dialog, m.as_start);
		continue;
	    };
	    auto j = d.find(id_dialog);
	    if(j == d.end()) {
		Dialog xd;
		xd.as_start = m.as_start;
		xd.as_end = m.as_end;
		xd.id_dialog = id_dialog;
		xd.tgt = m.tgt;
		xd.src = m.src;
		xd.cc = m.cc;
		xd.slot = m.slot;
		xd.mode = m.mode;
		xd.type = m.type;
		
		d[id_dialog] = xd;
		j = d.find(id_dialog);
	    };
	    
	    j->second.sq.push_back(&m);
	    
	    if(!j->second.alg_id && m.alg_id)
		j->second.alg_id = m.alg_id;

	    if(!j->second.key_id && m.key_id)
		j->second.key_id = m.key_id;

	    if(j->second.type.empty() && !m.type.empty())
		j->second.type = m.type;

	    if((j->second.mode != "VLC") && (m.mode == "VLC"))
		j->second.mode = "VLC";

	    bool audio_present = false;

//	    if((m.mode == "VLC") || (j->second.mode == "VLC")) {
	    if(m.mode == "VLC") {
//	    if(1) {
		auto af = audio_find_fragment(m.as_start, m.as_end, m.slot);
		if(af.samples) {
		    //unsigned last = j->second.as_end;
		    //unsigned curr = m.as_start;
		    //unsigned beep_count = (curr - last) / beep_int;
		
		    //if(j->second.audio.size())
			//while(beep_count--)
			//    j->second.add_audio(beep);
		    j->second.add_audio(af.data, af.samples);
		    audio_present = true;
		};
	    };
	    
	    if(!m.call_type.empty())
		j->second.call_type.insert(m.call_type);
	
	    add_set(j->second.ext_info, m.ext_info);

	    {
		double start_ut = ((double)m.as_start/48000.0) + signal_start_ut;
		char buf[16];
		sprintf(buf, "%.3f", start_ut);
		string s_start_ut(buf);
		
		double duration = (double)(m.as_end - m.as_start) / 48000.0;
		sprintf(buf, "%.3f", duration);
		string s_duration(buf);
		
		stringstream k;
		k << "{";
		k << " \"tgt\": " << m.tgt << ", \"src\": " << m.src << ", \"mode\": \"" << m.mode << "\"";
		k << ", \"audio\": " << (int)audio_present << ", ";
		k << "\"start_ut\": " << s_start_ut << ", \"duration\": " << s_duration << " , \"call_type\": \"" << m.call_type << "\" ";
		k << "}";
		j->second.session.push_back(k.str());
	    };

	    j->second.as_end = m.as_end;
	    LOG("DG[%d]: add %d-%d", id_dialog, m.as_start, m.as_end);
	};
    };

    LOG("DG: done - collecting");
    LOGF;

    string op;
    const char *out_pref = getenv("AVR_DSD_OUT_PREFIX");
    if(out_pref && *out_pref)
	op = *out_pref;

    if(!op.empty())
	op += "-";

    string tab = "       ";
    string tab1 = "          ";
    string tab2 = "             ";
    bool first = false;


    for(auto &x: d) {
	auto &q = x.second;
	if(first)
	    f << ",\n";
	else
	    first = true;

	char buf[48];
	
	f << tab << "{ \n";
	f << tab1;
	f << "\"id_dialog\": " << q.id_dialog << ", ";
	f << "\"user1\": " << q.tgt << ", ";
	f << "\"user2\": " << q.src << ", ";
	f << "\"mode\": \"" << q.mode << "\", ";
	f << "\"type\": \"" << q.type << "\", ";
	f << "\"standard\": \"" << sset_to_str(q.ext_info) <<"\",\n";
	f << tab1;

	f << "\"slot\": " << q.slot << ", ";
	f << "\"color\": " << q.cc << ", ";
	f << "\"alg_id\": \"" << to_hex(q.alg_id) << "\", ";
	f << "\"key_id\": \"" << to_hex(q.key_id) << "\", ";
	
	int encrypted = (q.alg_id && q.key_id);
	f << "\"encrypted\": \"" << encrypted << "\",\n";

	f << tab1;
	unsigned long start_ut = (unsigned long)((double)q.as_start/48000.0) + signal_start_ut;
	f << "\"start_ut\": " << start_ut << ", ";

        double duration = (double)(q.as_end - q.as_start) / 48000.0;
	sprintf(buf, "%.3f", duration);
	f << "\"duration\": " << string(buf) << ",\n";

	f << tab1;
	stringstream fnm_s;
//	op = dlg_tmp_prefix;
//	op += "-";

	sprintf(buf, "%02d", q.id_dialog);
	fnm_s << op << "D" << string(buf) << "_" << "s" << q.slot << "_" << q.tgt << "_" << q.src;
	string fnm = fnm_s.str();
	fnm += ".wav";
	
	string fnm_text = fnm_s.str();
	fnm_text += ".txt";
	
	if(q.audio.size() >= 4000) {
	    q.save_audio(fnm);
	}
	else
	    fnm.erase();
	
	f << "\"audio\": \"" << fnm << "\"";

	if(fnm.empty())
	    fnm_text.erase();
	f << ", \"text\": \"" << fnm_text << "\",\n";
	
	{	
	    f << tab1 << "\"session\": [";
	    
	    bool nempty = false;
	    for(const auto &x: q.session) {
		if(nempty)
		    f << ",";
		f << "\n" << tab2 << x;
		nempty = true;
	    };
	    
	    if(nempty)
		f << "\n";
	    f << tab1 << "]";
	};
	
	f << "\n";
	f<< tab << "}";
    };

    f << "\n";

    LOG("DG: done");
    LOGF;
};

// ----------------------

static void save_sessions(const string &filename)
{
    ::unlink(filename.c_str());
    ofstream f(filename, ios::out);
    if(!f)
	return;

    f << "{\n";
    f << "   \"session\": [\n";
    
    bool first = false;
    for(auto &x: *ss) {
	if(first) f << ",\n";
	first = true;
	f << x.to_string();
    };

    f << "\n   ],\n";
    
    f << "   \"dialog\": [\n";
    save_dialogs(f);
    f << "   ]\n";
    
    f << "}\n";
    LOG("Save sessions - done"); LOGF;
};


// ----------------------

struct OutFile {
    int fd = -1;
    string name;
    
    OutFile(const string &filename) : name(filename) { };
    
    ~OutFile() {
	if(fd != -1)
	    close(fd);
    };
    
    void send(const void *data, unsigned sz);
};

void OutFile::send(const void *data, unsigned sz)
{
    if(fd == -1) {
	::unlink(name.c_str());
	fd = open(name.c_str(), O_RDWR | O_CREAT, 0666);
    };
    
    if(fd != -1)
	write(fd, data, sz);
};


// ----------------------

static OutFile *pcm = 0;

struct Initializer {
    Log *log = 0;
    ofstream *ft = 0;

    Initializer() {
    
	const unsigned audo_samples_reserved = 8000 * 120;
	audio_slot1.reserve(audo_samples_reserved);
	audio_slot2.reserve(audo_samples_reserved);
	audio_index.reserve(audo_samples_reserved);
    
    
	wdir = new string;
	*wdir = ".";

	const char *sstart = getenv("AVR_DSD_SIGNAL_START_UT");
	if(sstart && *sstart) {
	    try {
		signal_start_ut = std::stoul(sstart);
	    }catch(...) { signal_start_ut = 0; };
	};

    
	const char *wd = getenv("AVR_DSD_WDIR");
	if(wd)
	    *wdir = wd;
	
	const char *dmr_check_flag = getenv("AVR_DSD_DMR_CHECK");
	dmr_check = (dmr_check_flag != 0);
	
	const char *log_name = getenv("AVR_DSD_EXT_LOG");
	if (log_name && *log_name)
	    log = new Log(log_name);

	const char *ss_name = getenv("AVR_DSD_SESSIONS");
	if (!dmr_check && ss_name && *ss_name) {
	    string ss_log_name = string(ss_name) + ".log";
	    ::unlink(ss_log_name.c_str());
	    auto llog = new ofstream(ss_log_name, ios::out);
	    flog = llog;
	};
	
	empty_string = new string;
	s1 = new PRecords;
	s2 = new PRecords;
	ss = new vector<Session>;
	
	const char *pcm_file = getenv("AVR_DSD_PCM_FILE");
	if(pcm_file && *pcm_file) {
	    dlg_tmp_prefix = pcm_file;
	    pcm = new OutFile(pcm_file);

	    pcm_index_file = pcm_file;
	    pcm_index_file += ".index";

	    
	    string ft_name = pcm_file;
	    ft_name += ".stat";
	    ::unlink(ft_name.c_str());
	    ft = new ofstream(ft_name, ios::out);
	};

	const char *beep_file = getenv("AVR_DSD_BEEP");
	if(beep_file && *beep_file) {
	    beep = read_pcm(beep_file);
	    if(beep.size())
		for(int i=0; i<800; ++i)
		    beep.push_back(0);
	};

	
    };

    ~Initializer() {
	delete ft;
    };
    
    operator bool() const { return (log != 0); };
    
};

Initializer L;

// ----------------------

static void avr_process_i(void)
{
    const char *ss_name = getenv("AVR_DSD_SESSIONS");
    if (!dmr_check && ss_name && *ss_name) {
	    
	detect();
	save_sessions(string(ss_name));
    };
    
    delete L.log;
    LOG("Delete event log."); LOGF;
    
    delete pcm;
    LOG("Delete pcm"); LOGF;



    if(flog) {
	LOG("Delete this log"); LOGF;
	delete flog;
    }
    
    if(audio_index.size())
	save_pcm_index(audio_index, pcm_index_file);
};


// ----------------------
// ----------------------
// ----------------------
// ----------------------





}; // avr_log

extern "C" {

void avr_process(void)
{
    avr::avr_process_i();
};



void ippl_new(const char *type)
{
    if(avr::L)
	avr::L.log->new_line(type);
};


void ippl_add(const char *key, const char *value)
{
    if(avr::L)
	avr::L.log->add(key, value);
};

void ippl_adds(const char *key, const char *value)
{
    if(avr::L)
	avr::L.log->adds(key, value);
};

void ippl_addi(const char *key, int value)
{
    if(avr::L)
	avr::L.log->add(key, value);
};

void ippl_addu(const char *key, unsigned value)
{
    if(avr::L)
	avr::L.log->add(key, value);
};

// ---

void ipp_to_pcm(const void *data, unsigned sz)
{
    if(avr::pcm && data && sz)
	avr::pcm->send(data, sz);

    avr::add_slot_audio(data, sz);

    unsigned int sample_num = avr::last_sample_num;

    double tm = (double)sample_num / 48000.0;
    char buf[32];
    sprintf(buf, "%.3f", tm);

    if(avr::L.ft)
	*avr::L.ft << sz << " : " << sample_num << " " << tm  << std::endl;
};

void ilog(const char *str)
{
    if(str)
	avr::log_printf("%s", str);
};

void ipp_last_sample_num(void)
{
    avr::last_sample_num = dmr_filter_sample_num;
};



};





