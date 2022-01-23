/*
 * FastCGI GDAL raster subsetter
 * 
 * 2021/02/24
 * Lucian Plesea
 * 
 */

#include <cstdio>
#include <string>
#include <sstream>
#include <unordered_map>
#include <fcgiapp.h>
#include <cgicc/Cgicc.h>

// Things like head(), body(), br(), streamable out
#include <cgicc/HTMLClasses.h>
#include <cgicc/HTTPHTMLHeader.h>
#include <cgicc/HTTPResponseHeader.h>
#include <cgicc/HTTPStatusHeader.h>

#include <gdal_priv.h>
#include <gdal_utils.h>

using namespace std;
using namespace cgicc;

// A wrapper around a string, to carry a GDAL visfilename
struct vsifname {
    vsifname(const char *filename) : name(filename) {};
    CPLString name;
};

struct Dynconf {
    string prefix;
    string suffix;
};

// A GDAL dataset, preopened if pds is not nullptr
struct gdataset {
    gdataset() : pds(nullptr), type(0) {};
    bool open(const string &fname) {
        if (fname == name) {
            cerr << "Same file\n";
            return pds != nullptr;
        }
        clear();
        name = fname;
        pds = GDALDataset::Open(fname.c_str(), GA_ReadOnly);
        return pds != nullptr;
    }

    void clear() {
        if (pds) {
            // Prefered over delete
            GDALClose(GDALDataset::ToHandle(pds));
            pds = nullptr;
            name.clear();
        }
    }

    GDALDataset *pds;
    string name;
    int type;
};

// CGIInput adapter class, for use with libfcgi
// need to define read() and getenv() to keep libcgicc happy
class State : public CgiInput {
public:
    State(FCGX_Request *request = nullptr) : verbose(false), req(request) {}

    ~State() {
        dataset.clear();
        if (conf)
            CSLDestroy(conf);
    }

    bool configure(char **config = nullptr);

    size_t read(char *data, size_t len) override {
        if (!req) return 0;
        return FCGX_GetStr(data, len, req->in);
    }

    std::string getenv(const char *varName) override {
        if (!req) return "";
        return string(CSLFetchNameValueDef(req->envp, varName, ""));
    }

    int send(const void *data, size_t len) {
        if (req && req->out)
            return FCGX_PutStr(reinterpret_cast<const char *>(data), len, req->out);
        else
            return fwrite(data, 1, len, stdout);
    }

    // Send a response as a string. Can be called once or multiple times per request
    int send(const string &response) {
        return send(response.c_str(), response.size());
    }

    // Send the content of a vector, usually byte
    template<typename T> int send(const vector<T> & response) {
        return send(response.data(), response.size() * sizeof(T));
    }

    // Send the content of a file, using VSI, return -1 for errors
    int send(const vsifname &fname) {
        VSIStatBufL statb;
        if (VSIStatL(fname.name, &statb) || statb.st_size == 0 || statb.st_size > 1024 * 1024 * 10)
            return -1;
        auto ofile = VSIFOpenL(fname.name, "rb");
        if (nullptr == ofile)
            return -1;
        vector<char> buffer(statb.st_size);
        VSIFReadL(buffer.data(), 1, statb.st_size, ofile);
        VSIFCloseL(ofile);
        return send(buffer);
    }

    bool verbose;
    // The fastcgi request object
    FCGX_Request *req;
    // Helper for CGI input
    Cgicc *cgi;
    // Input configuration file, line by line
    char **conf;
    // GDAL pre-open dataset
    gdataset dataset;
    // string prefix and suffix if ID is expected
    Dynconf dynconf;
};

// Maybe this should be a JSON file?
bool State::configure(char **config) {
    conf = config;
    if (strlen(CSLFetchNameValueDef(conf, "Filename", ""))) {
        // The input dataset, any error gets set to stderr
        if (!dataset.open(CSLFetchNameValueDef(conf, "Filename", ""))) {
            cerr << "Can't open file named \"" << dataset.name << '"' << endl;
            return false; // Failure to open
        }
        return true;
    } else { // Assume dynamic
        dataset.type = 1;
        dynconf.prefix = CSLFetchNameValueDef(conf, "DPrefix", "");
        dynconf.suffix = CSLFetchNameValueDef(conf, "DSuffix", "");
        return true;
    }

    return false;
}

// int usage(int argc, char **argv) {
//     fprintf(stderr, "Usage: %s\n [options] filename", argv[0]);
//     fprintf(stderr, "\t% -v : verbose\n");
//     return 1;
// }

const unordered_map<int, const char *> html_errors = {
    {400, "Bad Request"},
    {404 , "Not Found"}, 
    {500 , "Internal Server error"}, 
};

static int ret_error(State &c, const string &message, int code = 404) {
    ostringstream os;
    // With cgicc, only the Status line can be sent, which means no other headers work is raw text
    // os << HTTPStatusHeader(code, message);

    // Map to 404 if an invalid code is passed    
    if (html_errors.find(code) == html_errors.end())
        code = 404;

    os << "Status: " << code << " " << html_errors.at(code) << endl;
    os << "Content-type: text/html" << endl;
    os << endl;

    // can use tags from cgicc html
    os << html()
        << h1() << html_errors.at(code) << h1() << br() 
        << message  << br() 
        << html() << endl;

    c.send(os.str());
    return 0;
}

int get_missing(State &c) {
    auto conf = c.conf;
    auto request = c.req;
    auto &cgi = *c.cgi;

    string fname = CSLFetchNameValueDef(conf, "Missing", "");
    if (fname.empty())
        return ret_error(c, "Need missing file");

    FILE *missing = fopen(fname.c_str(), "rb");
    fseek(missing, 0, SEEK_END);
    size_t sz = ftell(missing);
    fseek(missing, 0, SEEK_SET);
    vector<unsigned char> buffer(sz);
    fread(buffer.data(), sz, 1, missing);

    ostringstream os;
    os << "Status: 200 OK\r\n";
    os << "Content-type: image/jpeg\r\n";
    os << "\r\n";
    c.send(os.str());
    c.send(buffer);

    return 0;
}

int parse_bbox(const char *bbval, double *bbox) {
    int i = 0;
    errno = 0;
    auto bb = const_cast<char *>(bbval);
    do {
        if (*bb == ',') bb++;
        bbox[i] = CPLStrtod(bb, &bb);
        if (errno)
            return i;
        i++;
    } while (i < 4 && * bb);
    if (i < 3)
        return i;
    // Check the basic bbox order
    if (bbox[2] <= bbox[0] || bbox[3] <= bbox[1])
        return bbox[2] <= bbox[0];
    return 4;
}

int get_image(State &c) {
    ostringstream os;    
    auto conf = c.conf;
    auto request = c.req;
    auto &cgi = *c.cgi;

    // Default sizes
    int xsz = 1024;
    int ysz = 1024;

    // Start accumulating the text
    if (c.verbose) {
        os << "Status: 200 OK\r\n";
        os << "Content-type: text/html\r\n";
        os << "\r\n";
    }

    if (cgi("size").empty())
        return ret_error(c, "Missing size parameter");
    if (2 != sscanf(cgi("size").c_str(), "%u,%u", &xsz, &ysz))
        return ret_error(c, "Can't parse size");

    // Limit the size
    if (xsz > 2048 || ysz > 2048)
        xsz = ysz = 1024;

    // Assume EPSG:4326
    double bbox[4] = { -180, -90, 180, 90 };
    if (!cgi("bbox").empty())
        if (4 != parse_bbox(cgi("bbox").c_str(), bbox))
            return ret_error(c, "Can't parse bbox");
    if (c.verbose) {
        os << "Bounding Box" <<
         bbox[0] << "," <<
         bbox[1] << "," <<
         bbox[2] << "," <<
         bbox[3] << "," <<
         br() << "\r\n";
    }

    if (!c.dataset.type) {
        // Like vargs
        char **targs = nullptr;
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "JPEG");
        targs = CSLAddString(targs, "-outsize");
        targs = CSLAddString(targs, CPLOPrintf("%u", xsz));
        targs = CSLAddString(targs, CPLOPrintf("%u", ysz));
        targs = CSLAddString(targs, "-projwin");
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[0]));
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[3]));
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[2]));
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[1]));
        targs = CSLAddString(targs, "0");

        auto topt = GDALTranslateOptionsNew(targs, nullptr);
        CPLFree(targs);
        int errv;
        const char outfname[] = "/vsimem/out.jpg";
        auto ods = GDALTranslate(outfname, c.dataset.pds, topt, &errv);
        GDALClose(ods); // Force flush
        GDALTranslateOptionsFree(topt);

        if (c.verbose) {
            c.send(os.str());
        }
        else {
            // Pass the "RAW" parameter to output raw image
            if (cgi("RAW").empty()) {
                os << "Status: 200 OK\r\n";
                os << "Content-type: image/jpeg\r\n";
                os << "\r\n";
                c.send(os.str());
            }
            c.send(vsifname(outfname));
        }

        VSIUnlink(outfname);
        // This is not strictly required, it will keep getting overwritten
        VSIUnlink(CPLOPrintf("%s%s", outfname, ".aux.xml"));
    } else if (!c.dynconf.prefix.empty()) {
        auto fname = cgi("ID");
        if (fname.empty())
            return ret_error(c, "Missing ID element", 400);
        fname = c.dynconf.prefix + fname + c.dynconf.suffix;
        if (!c.dataset.open(fname))
            return ret_error(c, "No such dataset");
        // Now it's open, check the bbox
        auto ds = c.dataset.pds;
        auto xsize = ds->GetRasterXSize();
        auto ysize = ds->GetRasterYSize();
        // Replace the default for bbox
        if (bbox[0] == -180 && bbox[1] == 180) {
            bbox[0] = 0;
            bbox[1] = 0;
            bbox[2] = xsize;
            bbox[3] = ysize;
        }
        for (int i=0; i < 4; i++)
            bbox[i] = static_cast<int>(bbox[i]);
        if (bbox[0] < 0 || bbox[1] < 0 || bbox[2] > xsize || bbox[3] > ysize)
            return ret_error(c, "Malformed bbox", 400);

        // Like vargs
        char **targs = nullptr;
        targs = CSLAddString(targs, "-of");
        targs = CSLAddString(targs, "JPEG");
        targs = CSLAddString(targs, "-outsize");
        targs = CSLAddString(targs, CPLOPrintf("%u", xsz));
        targs = CSLAddString(targs, CPLOPrintf("%u", ysz));
        targs = CSLAddString(targs, "-srcwin");
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[0]));
        targs = CSLAddString(targs, CPLSPrintf("%f", ysize - bbox[3]));
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[2] - bbox[0]));
        targs = CSLAddString(targs, CPLSPrintf("%f", bbox[3] - bbox[1]));
        targs = CSLAddString(targs, "0");

        auto topt = GDALTranslateOptionsNew(targs, nullptr );
        CPLFree(targs);
        int errv;
        const char outfname[] = "/vsimem/out.jpg";
        auto ods = GDALTranslate(outfname, c.dataset.pds, topt, &errv);
        GDALClose(ods); // Force flush
        GDALTranslateOptionsFree(topt);

        if (c.verbose) {
            c.send(os.str());
        }
        else {
            // Pass the "RAW" parameter to output raw image
            if (cgi("RAW").empty()) {
                os << "Status: 200 OK\r\n";
                os << "Content-type: image/jpeg\r\n";
                os << "\r\n";
                c.send(os.str());
            }
            c.send(vsifname(outfname));
        }

        VSIUnlink(outfname);
        // This is not strictly required, it will keep getting overwritten
        VSIUnlink(CPLOPrintf("%s%s", outfname, ".aux.xml"));
    } else { // Uknown configuration
        return ret_error(c, "Configuration failure", 500);
    }
    return 0;
}

int html_out(State &state, const string & extra) {
    auto request = state.req;
    auto &cgi = *state.cgi;
    // output string, as stream
    ostringstream os;

    // Output the HTTP headers for an HTML document, and the HTML 4.0 DTD info
    os << HTTPHTMLHeader() << HTMLDoctype(HTMLDoctype::eStrict) << endl;
    os << html().set("lang", "en").set("dir", "ltr") << endl;

    // Set up the page's header and title.
    os << head() << endl;
    os << title() << "GIS GDAL Image Subsetter" << title() << endl;
    os << head() << endl;

    // Start the HTML body
    os << body() << endl;

    // Print out a message
    os << h1("debug output") << endl;

    // also indirect, via cgi.getEnvironment() followed by specific get.. calls
    for (int i=0; request && request->envp[i]; i++) {
        os << "ENV" << i << " " << request->envp[i] << br() << endl;
    }

    // Unparsed in CGI mode
    if (!request) {
        os << "QUERY :" << cgi.getEnvironment().getQueryString();
    }

    // Form elements
    for (auto &i: *cgi)
        os << i.getName() << "=" << i.getValue() << br() << endl;

    // Or direct, by name
    if (cgi("bbox").empty())
        os << "Can't find bbox" << br() << endl;
    double bb[4];
    auto vals = parse_bbox(cgi("bbox").c_str(), bb);
    os << "Value " << vals <<  " x " << bb[0] << " y " << bb[1] << " X " << bb[2] << " Y " << bb[3] << endl;

    // Close the document
    os << body() << html();
    if (!extra.empty())
        os << "Extra " << extra << br() << endl;

    state.send(os.str());

    return 0;
}

int realmain(int argc, char **argv, char **env) {
    State state(nullptr);
    if (!state.configure(CSLLoad((string(argv[0]) + ".config").c_str()))) {
        GDALDestroy();
        return 1;
    }
    FCGX_Init();
    FCGX_Request request;
    FCGX_InitRequest(&request, 0, 0);

    auto is_fcgi = (FCGX_Accept_r(&request) >= 0);
    // Gets executed only once in CGI mode
    if (is_fcgi)
        state.req = &request;

    do { // Per request
        Cgicc cgi(is_fcgi ? & state : nullptr);
        state.cgi = &cgi;
        state.verbose = !cgi("dbg").empty();
        if (state.verbose)
            html_out(state, CPLOPrintf("%s %x %s", state.dataset.name.c_str(), state.dataset.pds, CPLGetLastErrorMsg() ));
        else
            get_image(state);
        if (is_fcgi)
            FCGX_Finish_r(&request);
    } while (is_fcgi && FCGX_Accept_r(&request) == 0);
}

int main(int argc, char **argv, char **env) {
    // All automatic GDAL structures have to be gone before calling Destroy()
    GDALAllRegister();
    realmain(argc, argv, env);
    GDALDestroy();
    return 0;
}