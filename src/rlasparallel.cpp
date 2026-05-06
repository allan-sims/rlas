/*
 * Parallel point-cloud reader for rlas.
 *
 * Each worker thread opens its own LASreader, seeks to its assigned point range,
 * and writes directly into pre-allocated R vectors via raw C pointers.
 * No R API calls are made inside worker threads.
 */
#include <Rcpp.h>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include "lasreader.hpp"

// LASlib's open() path has global state (message callback, static buffers).
// Serialize it across threads; the actual point-reading loop is fully parallel.
static std::mutex s_open_mutex;

using namespace Rcpp;

struct ReadOptions {
    bool extended;
    bool t, i, r, n, d, e, c, s, k, w, o, a, u, p, rgb, nir, cha;
};

struct OutputArrays {
    double* X;  double* Y;  double* Z;
    double* T;
    int*    I;
    int*    RN; int* NoR;
    int*    SDF; int* EoF;
    int*    C;
    int*    Synthetic; int* Keypoint; int* Withheld; int* Overlap;
    double* SA; int* SAR;
    int*    UD; int* PSI;
    int*    R;  int* G;  int* B;
    int*    NIR; int* Channel;
};

static void parallel_worker(const std::string& filename,
                              I64 start_pt, I64 end_pt, I64 offset,
                              const ReadOptions& opts,
                              OutputArrays& out,
                              std::string& err_out)
{
    try
    {
        LASreadOpener opener;
        opener.set_file_name(filename.c_str());
        opener.set_populate_header(true);
        LASreader* reader;
        {
            std::lock_guard<std::mutex> lk(s_open_mutex);
            reader = opener.open();
        }

        if (!reader) { err_out = "Could not open file"; return; }

        if (start_pt > 0 && !reader->seek(start_pt))
        {
            reader->close(); delete reader;
            err_out = "seek() failed at point " + std::to_string((long long)start_pt);
            return;
        }

        I64 count = end_pt - start_pt;
        for (I64 idx = 0; idx < count && reader->read_point(); idx++)
        {
            I64 i = offset + idx;
            out.X[i] = reader->point.get_x();
            out.Y[i] = reader->point.get_y();
            out.Z[i] = reader->point.get_z();
            if (opts.t) out.T[i] = reader->point.get_gps_time();
            if (opts.i) out.I[i] = reader->point.get_intensity();
            if (opts.r) out.RN[i]  = opts.extended ? reader->point.get_extended_return_number()
                                                    : reader->point.get_return_number();
            if (opts.n) out.NoR[i] = opts.extended ? reader->point.get_extended_number_of_returns()
                                                    : reader->point.get_number_of_returns();
            if (opts.d) out.SDF[i] = reader->point.get_scan_direction_flag();
            if (opts.e) out.EoF[i] = reader->point.get_edge_of_flight_line();
            if (opts.c) out.C[i]   = opts.extended ? reader->point.get_extended_classification()
                                                    : reader->point.get_classification();
            if (opts.s) out.Synthetic[i] = reader->point.get_synthetic_flag();
            if (opts.k) out.Keypoint[i]  = reader->point.get_keypoint_flag();
            if (opts.w) out.Withheld[i]  = reader->point.get_withheld_flag();
            if (opts.o) out.Overlap[i]   = reader->point.get_extended_overlap_flag();
            if (opts.a)
            {
                if (opts.extended) out.SA[i]  = reader->point.get_scan_angle();
                else               out.SAR[i] = reader->point.get_scan_angle_rank();
            }
            if (opts.u) out.UD[i]  = reader->point.get_user_data();
            if (opts.p) out.PSI[i] = reader->point.get_point_source_ID();
            if (opts.rgb)
            {
                out.R[i] = reader->point.get_R();
                out.G[i] = reader->point.get_G();
                out.B[i] = reader->point.get_B();
            }
            if (opts.nir) out.NIR[i]     = reader->point.get_NIR();
            if (opts.cha) out.Channel[i] = reader->point.get_extended_scanner_channel();
        }

        reader->close();
        delete reader;
    }
    catch (std::exception& e)
    {
        err_out = e.what();
    }
}

// [[Rcpp::export]]
List C_reader_parallel(CharacterVector ifiles, CharacterVector select_cv, int nthreads)
{
    std::string filename = as<std::string>(ifiles[0]);

    // Open one reader just to inspect the header
    LASreadOpener hopener;
    hopener.set_populate_header(true);
    hopener.set_file_name(filename.c_str());
    LASreader* hreader = hopener.open();
    if (!hreader) stop("Could not open file");

    U8  point_type = hreader->header.point_data_format;
    bool extended  = (hreader->header.version_minor >= 4) && (point_type >= 6);

    I64 npoints = (I64)hreader->header.number_of_point_records;
    if (npoints == 0 && hreader->header.version_minor >= 4)
        npoints = (I64)hreader->header.extended_number_of_point_records;

    bool has_rgb = (point_type == 2 || point_type == 3 || point_type == 5 ||
                    point_type == 7 || point_type == 8 || point_type == 10);
    bool has_t   = (point_type == 1 || point_type >= 3);
    bool has_nir = (point_type == 8 || point_type == 10);

    hreader->close();
    delete hreader;

    if (npoints <= 0) stop("File reports 0 points");

    // Parse select string (same logic as RLASstreamer::select(), minus W and extra bytes)
    std::string sel = as<std::string>(select_cv[0]);
    if (sel.find("*") != std::string::npos)
        sel = "xyztirndecCskwoaupRGBN";

    ReadOptions opts;
    opts.extended = extended;
    opts.t   = sel.find("t") != std::string::npos && has_t;
    opts.i   = sel.find("i") != std::string::npos;
    opts.r   = sel.find("r") != std::string::npos;
    opts.n   = sel.find("n") != std::string::npos;
    opts.d   = sel.find("d") != std::string::npos;
    opts.e   = sel.find("e") != std::string::npos;
    opts.c   = sel.find("c") != std::string::npos;
    opts.s   = sel.find("s") != std::string::npos;
    opts.k   = sel.find("k") != std::string::npos;
    opts.w   = sel.find("w") != std::string::npos;
    opts.o   = sel.find("o") != std::string::npos && extended;
    opts.a   = sel.find("a") != std::string::npos;
    opts.u   = sel.find("u") != std::string::npos;
    opts.p   = sel.find("p") != std::string::npos;
    opts.rgb = (sel.find("R") != std::string::npos || sel.find("G") != std::string::npos ||
                sel.find("B") != std::string::npos) && has_rgb;
    opts.nir = sel.find("N") != std::string::npos && has_nir;
    opts.cha = sel.find("C") != std::string::npos && extended;

    // Pre-allocate output vectors in R memory; threads write via raw pointers
    R_xlen_t n = (R_xlen_t)npoints;

    NumericVector Xv(n), Yv(n), Zv(n);
    NumericVector Tv;
    IntegerVector Iv, RNv, NoRv, SDFv, EoFv, Cv;
    IntegerVector Synv, Keyv, Withv, Ovlv;
    NumericVector SAv; IntegerVector SARv;
    IntegerVector UDv, PSIv;
    IntegerVector Rv, Gv, Bv, NIRv, Chanv;

    if (opts.t)   Tv   = NumericVector(n);
    if (opts.i)   Iv   = IntegerVector(n);
    if (opts.r)   RNv  = IntegerVector(n);
    if (opts.n)   NoRv = IntegerVector(n);
    if (opts.d)   SDFv = IntegerVector(n);
    if (opts.e)   EoFv = IntegerVector(n);
    if (opts.c)   Cv   = IntegerVector(n);
    if (opts.s)   Synv = IntegerVector(n);
    if (opts.k)   Keyv = IntegerVector(n);
    if (opts.w)   Withv = IntegerVector(n);
    if (opts.o)   Ovlv  = IntegerVector(n);
    if (opts.a) { if (opts.extended) SAv = NumericVector(n); else SARv = IntegerVector(n); }
    if (opts.u)   UDv  = IntegerVector(n);
    if (opts.p)   PSIv = IntegerVector(n);
    if (opts.rgb) { Rv = IntegerVector(n); Gv = IntegerVector(n); Bv = IntegerVector(n); }
    if (opts.nir) NIRv  = IntegerVector(n);
    if (opts.cha) Chanv = IntegerVector(n);

    OutputArrays out = {};
    out.X = REAL(Xv); out.Y = REAL(Yv); out.Z = REAL(Zv);
    if (opts.t)   out.T   = REAL(Tv);
    if (opts.i)   out.I   = INTEGER(Iv);
    if (opts.r)   out.RN  = INTEGER(RNv);
    if (opts.n)   out.NoR = INTEGER(NoRv);
    if (opts.d)   out.SDF = INTEGER(SDFv);
    if (opts.e)   out.EoF = INTEGER(EoFv);
    if (opts.c)   out.C   = INTEGER(Cv);
    if (opts.s)   out.Synthetic = INTEGER(Synv);
    if (opts.k)   out.Keypoint  = INTEGER(Keyv);
    if (opts.w)   out.Withheld  = INTEGER(Withv);
    if (opts.o)   out.Overlap   = INTEGER(Ovlv);
    if (opts.a) { if (opts.extended) out.SA = REAL(SAv); else out.SAR = INTEGER(SARv); }
    if (opts.u)   out.UD  = INTEGER(UDv);
    if (opts.p)   out.PSI = INTEGER(PSIv);
    if (opts.rgb) { out.R = INTEGER(Rv); out.G = INTEGER(Gv); out.B = INTEGER(Bv); }
    if (opts.nir) out.NIR     = INTEGER(NIRv);
    if (opts.cha) out.Channel = INTEGER(Chanv);

    // Divide work and launch threads
    int actual_threads = std::max(1, std::min(nthreads, (int)npoints));
    std::vector<std::thread> threads;
    threads.reserve(actual_threads);
    std::vector<std::string> errors(actual_threads);

    I64 pts_per_thread = npoints / actual_threads;
    for (int t = 0; t < actual_threads; t++)
    {
        I64 start = (I64)t * pts_per_thread;
        I64 end   = (t == actual_threads - 1) ? npoints : start + pts_per_thread;
        threads.emplace_back(parallel_worker,
                             filename, start, end, start,
                             std::cref(opts), std::ref(out), std::ref(errors[t]));
    }

    for (auto& th : threads) th.join();

    for (int t = 0; t < actual_threads; t++)
        if (!errors[t].empty())
            stop("Thread " + std::to_string(t) + ": " + errors[t]);

    // Assemble result list (same structure expected by read_and_write.las)
    List lasdata = List::create(Xv, Yv, Zv);
    CharacterVector attr_name = CharacterVector::create("X", "Y", "Z");

    if (opts.t)   { lasdata.push_back(Tv);    attr_name.push_back("gpstime"); }
    if (opts.i)   { lasdata.push_back(Iv);    attr_name.push_back("Intensity"); }
    if (opts.r)   { lasdata.push_back(RNv);   attr_name.push_back("ReturnNumber"); }
    if (opts.n)   { lasdata.push_back(NoRv);  attr_name.push_back("NumberOfReturns"); }
    if (opts.d)   { lasdata.push_back(SDFv);  attr_name.push_back("ScanDirectionFlag"); }
    if (opts.e)   { lasdata.push_back(EoFv);  attr_name.push_back("EdgeOfFlightline"); }
    if (opts.c)   { lasdata.push_back(Cv);    attr_name.push_back("Classification"); }
    if (opts.cha) { lasdata.push_back(Chanv); attr_name.push_back("ScannerChannel"); }
    if (opts.s)   { lasdata.push_back(Synv);  attr_name.push_back("Synthetic_flag"); }
    if (opts.k)   { lasdata.push_back(Keyv);  attr_name.push_back("Keypoint_flag"); }
    if (opts.w)   { lasdata.push_back(Withv); attr_name.push_back("Withheld_flag"); }
    if (opts.o)   { lasdata.push_back(Ovlv);  attr_name.push_back("Overlap_flag"); }
    if (opts.a)
    {
        if (opts.extended) { lasdata.push_back(SAv);  attr_name.push_back("ScanAngle"); }
        else               { lasdata.push_back(SARv); attr_name.push_back("ScanAngleRank"); }
    }
    if (opts.u)   { lasdata.push_back(UDv);  attr_name.push_back("UserData"); }
    if (opts.p)   { lasdata.push_back(PSIv); attr_name.push_back("PointSourceID"); }
    if (opts.rgb)
    {
        lasdata.push_back(Rv); attr_name.push_back("R");
        lasdata.push_back(Gv); attr_name.push_back("G");
        lasdata.push_back(Bv); attr_name.push_back("B");
    }
    if (opts.nir) { lasdata.push_back(NIRv); attr_name.push_back("NIR"); }

    lasdata.names() = attr_name;
    return lasdata;
}
