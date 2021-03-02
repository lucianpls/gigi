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

// CGIInput adapter class, for use with libfcgi
// need to define read() and getenv() to keep libcgicc happy
class state : public CgiInput {
public:
    state(FCGX_Request *request) : verbose(false), req(request) {}

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
    GDALDataset *pds;
};

// int usage(int argc, char **argv) {
//     fprintf(stderr, "Usage: %s\n [options] filename", argv[0]);
//     fprintf(stderr, "\t% -v : verbose\n");
//     return 1;
// }

const unordered_map<int, const char *> html_errors = {
    { 404 , "Not Found"},
};

int ret_error(state &c, const string &message, int code = 404) {
    ostringstream os;    

    // With cgicc, only the Status line can be sent, which means no other headers work is raw text
    // os << HTTPStatusHeader(code, message);

    // Map to 404 if an invalid code is passed    
    if (html_errors.find(code) != html_errors.end())
        code = 404;

    os << "Status: " << code << " " << html_errors.at(code) << endl;
    os << "Content-type: text/html" << endl;
    os << endl;

    // can use tags from cgicc html
    os << html() << h1() << message  << h1() << br() << html() << endl;

    c.send(os.str());
    return 0;
}

int get_missing(state &c) {
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
    char *bb;
    errno = 0;
    *bbox++ = CPLStrtod(bbval, &bb);
    if (errno)
        return 0;
    i++;
    do {
        if (*bb == ',') bb++;
        *bbox++ = CPLStrtod(bb, &bb);
        if (errno)
            return i;
        i++;
    } while (i < 4 && *bbval);
    return i;
}

int get_image(state &c) {
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

    auto topt = GDALTranslateOptionsNew(targs, nullptr );
    CPLFree(targs);
    int errv;
    const char outfname[] = "/vsimem/out.jpg";
    auto ods = GDALTranslate(outfname, c.pds, topt, &errv);
    GDALClose(ods); // Force flush
    GDALTranslateOptionsFree(topt);

    if (c.verbose) {
        c.send(os.str());
    }
    else {
        os << "Status: 200 OK\r\n";
        os << "Content-type: image/jpeg\r\n";
        os << "\r\n";
        c.send(os.str());
        c.send(vsifname(outfname));
    }

    VSIUnlink(outfname);
    // This is not strictly required, it will keep getting overwritten
    VSIUnlink(CPLOPrintf("%s%s", outfname, ".aux.xml"));
    return 0;
}

int html_out(state &c, const string & extra) {
    auto request = c.req;
    auto &cgi = *c.cgi;
    // output string, as stream
    ostringstream os;

    // Output the HTTP headers for an HTML document, and the HTML 4.0 DTD info
    os << HTTPHTMLHeader() << HTMLDoctype(HTMLDoctype::eStrict) << endl;
    os << html().set("lang", "en").set("dir", "ltr") << endl;

    // Set up the page's header and title.
    os << head() << endl;
    os << title() << "GNU cgicc v" << cgi.getVersion() << title() << endl;
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

    vector<FormEntry> res;
    os << br() << "Value of a is " << cgi.getElement("dbg", res) << br() << endl;
    // Or direct, by name
    if (cgi("bbox").empty())
        os << "Can't find bbox" << br() << endl;

    // Close the document
    os << body() << html();
    if (!extra.empty())
        os << "Extra " << extra << br() << endl;

    c.send(os.str());

    return 0;
}

int main(int argc, char **argv, char **env) {
    string confname(argv[0]);
    confname += ".config";
    auto conf = CSLLoad(confname.c_str());

    string fname = CSLFetchNameValueDef(conf, "FileName", "");
    GDALAllRegister();
    
    // The input dataset, any error gets set to stderr
    auto pds = GDALDataset::Open(fname.c_str(), GA_ReadOnly);
    // if (!pds) {
    //     cerr << "Can't open file named \"" << fname << '"' << endl;
    //     return 1; // Failure to open
    // }

    FCGX_Init();
    FCGX_Request request;
    FCGX_InitRequest(&request, 0, 0);

    auto is_fcgi = (FCGX_Accept_r(&request) >= 0);
    // Gets executed only once in CGI mode
    state c(is_fcgi ? &request : nullptr);
    c.conf = conf;
    c.pds = pds;
    do {
        // Per request
        Cgicc cgi(is_fcgi ? &c : nullptr);
        c.cgi = &cgi;

        vector<FormEntry> res;
        c.verbose = cgi.getElement("dbg", res);
        // if (c.verbose)
        //     html_out(c, CPLOPrintf("%x %s", pds, CPLGetLastErrorMsg() ));
        // else
        get_image(c);
        if (is_fcgi)
            FCGX_Finish_r(&request);

    } while (is_fcgi && FCGX_Accept_r(&request) == 0);

    // Prefered over delete
    GDALClose(GDALDataset::ToHandle(pds));
    CPLFree(conf);
    GDALDestroy();
   return 0;
}