/*
 * FastCGI GDAL raster subsetter
 * 
 * 2021/02/24
 * Lucian Plesea
 * 
 */

#include <string>
#include <sstream>
#include <fcgiapp.h>
#include <cgicc/Cgicc.h>

// Things like head(), body(), br(), streamable out
#include <cgicc/HTMLClasses.h>
#include <cgicc/HTTPHTMLHeader.h>

#include <gdal_priv.h>

using namespace std;
using namespace cgicc;

// Minimal CGIIO adapter class, for use with libfcgi
class myCgiI : public CgiInput {
public:
    myCgiI(FCGX_Request *request) : req(request) {}

    size_t read(char *data, size_t len) override {
        if (!req) return 0;
        return FCGX_GetStr(data, len, req->in);
    }

    std::string getenv(const char *varName) override {
        if (!req) return "";
        return string(CSLFetchNameValueDef(req->envp, varName, ""));
    }

    FCGX_Request *req;
};

// int usage(int argc, char **argv) {
//     fprintf(stderr, "Usage: %s\n [options] filename", argv[0]);
//     fprintf(stderr, "\t% -v : verbose\n");
//     return 1;
// }

int get_image(myCgiI &mcgi, Cgicc &cgi) {
    return 0;
}

int html_out(myCgiI &mcgi, Cgicc &cgi, const string & extra) {
    auto request = mcgi.req;
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

    if (request) {
        FCGX_PutStr(os.str().c_str(), os.str().size(), request->out);
    } else {
        cout << os.str();
    }

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
    do {
        myCgiI cgiI(is_fcgi ? &request : nullptr);
        Cgicc cgi(is_fcgi ? &cgiI : nullptr);

        vector<FormEntry> res;
        if (cgi.getElement("dbg", res))
            html_out(cgiI, cgi, CPLOPrintf("%x %s", pds, CPLGetLastErrorMsg() ));
        else
            get_image(cgiI, cgi);

        if (is_fcgi)
            FCGX_Finish_r(&request);

    } while (is_fcgi && FCGX_Accept_r(&request) == 0);

    delete pds;
    CPLFree(conf);
    GDALDestroy();
   return 0;
}