// Copyright (c) 2014-2015, Pacific Biosciences of California, Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted (subject to the limitations in the
// disclaimer below) provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//  * Neither the name of Pacific Biosciences nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
// GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY PACIFIC
// BIOSCIENCES AND ITS CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL PACIFIC BIOSCIENCES OR ITS
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
// USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
// OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

// Author: Lance Hepler

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/join.hpp>
#include <boost/optional.hpp>

#include <OptionParser.h>

#include <pbbam/BamWriter.h>
#include <pbbam/EntireFileQuery.h>
#include <pbbam/PbiBuilder.h>

#include <pacbio/ccs/Consensus.h>
#include <pacbio/ccs/ExecUtils.h>
#include <pacbio/ccs/Interval.h>
#include <pacbio/ccs/Logging.h>
#include <pacbio/ccs/ReadId.h>
#include <pacbio/ccs/Whitelist.h>
#include <pacbio/ccs/WorkQueue.h>
#include <pacbio/ccs/Utility.h>

using namespace std;
using namespace PacBio::BAM;
using namespace PacBio::CCS;

using boost::none;
using boost::optional;
using optparse::OptionParser;


// these strings are part of the BAM header, they CANNOT contain newlines
#define VERSION "0.0.1"
#define DESCRIPTION "Generate circular consensus sequences (ccs) from subreads."


namespace PacBio {
namespace CCS {
namespace OptionNames {
    constexpr auto ForceOutput  = "force";
    constexpr auto PbIndex      = "pbi";
    constexpr auto Zmws         = "zmws";
    constexpr auto MinSnr       = "minSnr";
    constexpr auto MinReadScore = "minReadScore";
    constexpr auto ReportFile   = "reportFile";
    constexpr auto NumThreads   = "numThreads";
    constexpr auto LogFile      = "logFile";
    constexpr auto LogLevel     = "logLevel";
} // namespace OptionNames
} // namespace CCS
} // namespace PacBio


typedef ReadType<ReadId>           Subread;
typedef ChunkType<ReadId, Subread> Chunk;
typedef ConsensusType<ReadId>      CCS;
typedef ResultType<CCS>            Results;


const auto CircularConsensus = &Consensus<Chunk, CCS>;


void Writer(BamWriter& ccsBam, unique_ptr<PbiBuilder>& ccsPbi, Results& counts, Results&& results)
{
    counts += results;

    for (const auto& ccs : results)
    {
        BamRecordImpl record;
        TagCollection tags;
        stringstream  name;

        // some defaults values
        record.Bin(0)
              .InsertSize(0)
              .MapQuality(255)
              .MatePosition(-1)
              .MateReferenceId(-1)
              .Position(-1)
              .ReferenceId(-1)
              .Flag(0)
              .SetMapped(false);

        name << *(ccs.Id.MovieName) << '/' << ccs.Id.HoleNumber << "/ccs";

        vector<float> snr = {
            static_cast<float>(ccs.SignalToNoise.A),
            static_cast<float>(ccs.SignalToNoise.C),
            static_cast<float>(ccs.SignalToNoise.G),
            static_cast<float>(ccs.SignalToNoise.T) };

        tags["RG"] = MakeReadGroupId(*(ccs.Id.MovieName), "CCS");
        tags["zm"] = static_cast<int32_t>(ccs.Id.HoleNumber);
        tags["np"] = static_cast<int32_t>(ccs.NumPasses);
        tags["rq"] = static_cast<int32_t>(1000 * ccs.PredictedAccuracy);
        tags["sn"] = snr;

        // TODO(lhepler) maybe remove one day
        tags["pq"] = static_cast<float>(ccs.PredictedAccuracy);
        tags["za"] = static_cast<float>(ccs.AvgZScore);
        vector<float> zScores;
        for (const double z : ccs.ZScores)
            zScores.emplace_back(static_cast<float>(z));
        tags["zs"] = zScores;
        tags["rs"] = ccs.StatusCounts;

#if 0
        // TODO(lhepler) remove these before release
        tags["ms"] = ccs.ElapsedMilliseconds;
        tags["mt"] = static_cast<int32_t>(ccs.MutationsTested);
        tags["ma"] = static_cast<int32_t>(ccs.MutationsApplied);


        // These are SUPER valuable for filtering, let's leave em in for now.
        tags["zg"] = static_cast<float>(ccs.GlobalZScore);

#endif

        record.Name(name.str())
              .SetSequenceAndQualities(ccs.Sequence, ccs.Qualities)
              .Tags(tags);

        int64_t offset;
        ccsBam.Write(record, &offset);

        if (ccsPbi)
            ccsPbi->AddRecord(record, offset);
    }
    ccsBam.TryFlush();
}


Results WriterThread(WorkQueue<Results>& queue, BamWriter& ccsBam, unique_ptr<PbiBuilder>& ccsPbi)
{
    Results counts;
    while (queue.ConsumeWith(Writer, ref(ccsBam), ref(ccsPbi), ref(counts)));
    return counts;
}


BamHeader PrepareHeader(const OptionParser& parser, int argc, char **argv, const vector<string>& files)
{
    using boost::algorithm::join;

    ProgramInfo program(parser.prog() + "-" + VERSION);
    program.Name(parser.prog())
           .CommandLine(parser.prog() + " " + join(vector<string>(argv + 1, argv + argc), " "))
           .Description(DESCRIPTION)
           .Version(VERSION);

    BamHeader header;
    header.PacBioBamVersion("3.0b7")
          .SortOrder("unknown")
          .Version("1.5")
          .AddProgram(program);

    for (auto file = ++files.begin(); file != files.end(); ++file)
    {
        BamFile bam(*file);

        for (const auto& rg : bam.Header().ReadGroups())
        {
            if (rg.ReadType() != "SUBREAD")
                parser.error("invalid input file, READTYPE must be SUBREAD");

            ReadGroupInfo readGroup(rg.MovieName(), "CCS");
            readGroup.BindingKit(rg.BindingKit())
                     .SequencingKit(rg.SequencingKit())
                     .BasecallerVersion(rg.BasecallerVersion())
                     .FrameRateHz(rg.FrameRateHz());

            header.AddReadGroup(readGroup);
        }
    }

    return header;
}


size_t ThreadCount(int n)
{
    const int m = thread::hardware_concurrency();

    if (n < 1)
        return max(1, m + n);

    return min(m, n);
}


void WriteResultsReport(ostream& report, const Results& counts)
{
    size_t total = counts.Total();

    report << fixed << setprecision(2);

    report << "Success -- CCS generated," << counts.Success
           << "," << 100.0 * counts.Success / total << '%' << endl;

    report << "Failed -- Below SNR threshold," << counts.PoorSNR
           << "," << 100.0 * counts.PoorSNR / total << '%' << endl;

    report << "Failed -- No usable subreads," << counts.NoSubreads
           << "," << 100.0 * counts.NoSubreads / total << '%' << endl;

    report << "Failed -- Insert size too small," << counts.TooShort
           << "," << 100.0 * counts.TooShort / total << '%' << endl;

    report << "Failed -- Not enough full passes," << counts.TooFewPasses
           << "," << 100.0 * counts.TooFewPasses / total << '%' << endl;

    report << "Failed -- Too many unusable subreads," << counts.TooManyUnusable
           << "," << 100.0 * counts.TooManyUnusable / total << '%' << endl;

    report << "Failed -- CCS did not converge," << counts.NonConvergent
           << "," << 100.0 * counts.NonConvergent / total << '%' << endl;

    report << "Failed -- CCS below minimum predicted accuracy," << counts.PoorQuality
           << "," << 100.0 * counts.PoorQuality / total << '%' << endl;
}


// TODO(lhepler) remove in favor of decoding, only accept P6/C4 data for now
bool VerifyChemistry(const ReadGroupInfo& readGroup)
{
    string bcVer = readGroup.BasecallerVersion().substr(0, 3);

    if (readGroup.BindingKit() == "100356300" &&
        readGroup.SequencingKit() == "100356200" &&
        (bcVer == "2.1" || bcVer == "2.3"))
        return true;

    if (readGroup.BindingKit() == "100372700" &&
        readGroup.SequencingKit() == "100356200" &&
        (bcVer == "2.1" || bcVer == "2.3"))
        return true;

    return false;
}


int main(int argc, char **argv)
{
    using boost::make_optional;

    SetColumns();

    // args and options
    //
    //
    auto parser = OptionParser()
        .usage("usage: %prog [OPTIONS] OUTPUT FILES...")
        .version("%prog " VERSION "\nCopyright (c) 2014-2015 Pacific Biosciences, Inc.\nLicense: 3-BSD")
        .description(DESCRIPTION "\nAdditional documentation: http://github.com/PacificBiosciences/pbccs");

    const vector<string> logLevels = { "TRACE", "DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "CRITICAL", "FATAL" };
    const string em = "--";

    parser.add_option(em + OptionNames::ForceOutput).action("store_true").help("Overwrite OUTPUT file if present.");
    parser.add_option(em + OptionNames::PbIndex).action("store_true").help("Generate a .pbi file for the OUTPUT file.");
    parser.add_option(em + OptionNames::Zmws).help("Generate CCS for the provided comma-separated holenumber ranges only. Default = all");
    parser.add_option(em + OptionNames::MinSnr).type("float").set_default(4).help("Minimum SNR of input subreads. Default = %default");
    parser.add_option(em + OptionNames::MinReadScore).type("float").set_default(0.75).help("Minimum read score of input subreads. Default = %default");

    ConsensusSettings::AddOptions(&parser);

    parser.add_option(em + OptionNames::ReportFile).set_default("ccs_report.csv").help("Where to write the results report. Default = %default");
    parser.add_option(em + OptionNames::NumThreads).type("int").set_default(0).help("Number of threads to use, 0 means autodetection. Default = %default");
    // parser.add_option("--chunkSize").type("int").set_default(5).help("Number of CCS jobs to submit simultaneously. Default = %default");
    parser.add_option(em + OptionNames::LogFile).help("Log to a file, instead of STDERR.");
    parser.add_option(em + OptionNames::LogLevel).choices(logLevels.begin(), logLevels.end()).set_default("INFO").help("Set log level. Default = %default");

    const auto options = parser.parse_args(argc, argv);
    const auto files   = parser.args();

    const ConsensusSettings settings(options);

    const bool forceOutput   = options.get(OptionNames::ForceOutput);
    const bool pbIndex       = options.get(OptionNames::PbIndex);
    const float minSnr       = options.get(OptionNames::MinSnr);
    const float minReadScore = 1000 * static_cast<float>(options.get(OptionNames::MinReadScore));
    const size_t nThreads    = ThreadCount(options.get(OptionNames::NumThreads));
    const size_t chunkSize   = 1;  // static_cast<size_t>(options.get("chunkSize"));

    if (static_cast<int>(options.get(OptionNames::MinPasses)) < 1)
        parser.error("option --minPasses: invalid value: must be >= 1");

    // handle --zmws
    //
    //
    optional<Whitelist> whitelist(none);
    const string wlspec(options.get(OptionNames::Zmws));
    try
    {
        if (!wlspec.empty())
            whitelist = Whitelist(wlspec);
    }
    catch (...)
    {
        parser.error("option --zmws: invalid specification: '" + wlspec + "'");
    }

    // input validation
    //
    //
    if (files.size() < 1)
        parser.error("missing OUTPUT");
    else if (files.size() < 2)
        parser.error("missing FILES...");

    // Verify output file does not already exist
    if (FileExists(files.front()) && !forceOutput)
        parser.error("OUTPUT: file already exists: '" + files.front() + "'");

    // logging
    //
    //
    fstream logStream;
    {
        string logLevel(options.get(OptionNames::LogLevel));
        string logFile(options.get(OptionNames::LogFile));

        if (!logFile.empty())
        {
            logStream.open(logFile, fstream::out);
            Logging::Logger::Default(new Logging::Logger(logStream, logLevel));
        }
        else
        {
            Logging::Logger::Default(new Logging::Logger(cerr, logLevel));
        }
        Logging::InstallSignalHandlers();
    }

    // start processing chunks!
    //
    //
    BamWriter ccsBam(files.front(), PrepareHeader(parser, argc, argv, files));
    unique_ptr<PbiBuilder> ccsPbi(nullptr);
    unique_ptr<vector<Chunk>> chunk(new vector<Chunk>());
    map<string, shared_ptr<string>> movieNames;

    if (pbIndex)
        ccsPbi.reset(new PbiBuilder(files.front() + ".pbi"));

    WorkQueue<Results> workQueue(nThreads);
    future<Results> writer = async(launch::async, WriterThread, ref(workQueue), ref(ccsBam), ref(ccsPbi));
    size_t poorSNR = 0, tooFewPasses = 0;

    // skip the first file, it's for output
    for (auto file = ++files.begin(); file != files.end(); ++file)
    {
        EntireFileQuery query(*file);

        // use make_optional here to get around spurious warnings from gcc:
        //   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=47679
        optional<int32_t> holeNumber(none);
        bool skipZmw = false;

        for (const auto& read : query)
        {
            const string movieName = read.MovieName();

            if (movieNames.find(movieName) == movieNames.end())
            {
                movieNames[movieName] = make_shared<string>(movieName);
            }

            if (!holeNumber || *holeNumber != read.HoleNumber())
            {
                if (chunk && !chunk->empty() && chunk->back().Reads.size() < settings.MinPasses)
                {
                    PBLOG_DEBUG << "Skipping ZMW " << chunk->back().Id
                                << ", insufficient number of passes ("
                                << chunk->back().Reads.size() << '<' << settings.MinPasses << ')';
                    tooFewPasses += 1;
                    chunk->pop_back();
                }

                if (chunk && chunk->size() >= chunkSize)
                {
                    workQueue.ProduceWith(CircularConsensus, move(chunk), settings);
                    chunk.reset(new vector<Chunk>());
                }

                holeNumber = read.HoleNumber();
                auto snr = read.SignalToNoise();
                if (whitelist && !whitelist->Contains(movieName, *holeNumber))
                {
                    skipZmw = true;
                }
                // test chemistry here, we only accept P6/C4 for now
                //   TODO(lhepler) remove this in favor of actual chemistry decoding
                else if (!VerifyChemistry(read.ReadGroup()))
                {
                    PBLOG_NOTICE << "Skipping ZMW " << movieName << '/' << *holeNumber << ", invalid chemistry (not P6/C4)";
                    skipZmw = true;
                }
                else if (*min_element(snr.begin(), snr.end()) < minSnr)
                {
                    PBLOG_DEBUG << "Skipping ZMW " << movieName << '/' << *holeNumber << ", fails SNR threshold (" << minSnr << ')';
                    poorSNR += 1;
                    skipZmw = true;
                }
                else
                {
                    skipZmw = false;
                    chunk->emplace_back(
                        Chunk
                        {
                            ReadId(movieNames[movieName], *holeNumber),
                            vector<Subread>(),
                            SNR(snr)
                        });
                }
            }

            if (skipZmw)
                continue;

            if (static_cast<float>(read.ReadAccuracy()) < minReadScore)
            {
                PBLOG_DEBUG << "Skipping read " << read.FullName()
                            << ", insufficient read accuracy ("
                            << read.ReadAccuracy() << '<' << minReadScore << ')';
                continue;
            }

            chunk->back().Reads.emplace_back(
                Subread
                {
                    ReadId(movieNames[movieName], *holeNumber, Interval(read.QueryStart(), read.QueryEnd())),
                    read.Sequence(),
                    read.LocalContextFlags(),
                    read.ReadAccuracy()
                });
        }
    }

    // if the last chunk doesn't have enough passes, skip it
    if (chunk && !chunk->empty() && chunk->back().Reads.size() < settings.MinPasses)
    {
        PBLOG_DEBUG << "Skipping ZMW " << chunk->back().Id
                    << ", insufficient number of passes ("
                    << chunk->back().Reads.size() << '<' << settings.MinPasses << ')';
        tooFewPasses += 1;
        chunk->pop_back();
    }

    // run the remaining tasks
    if (chunk && !chunk->empty())
    {
        workQueue.ProduceWith(CircularConsensus, move(chunk), settings);
    }

    // wait for the queue to be done
    workQueue.Finalize();

    // wait for the writer thread and get the results counter
    //   then add in the snr/minPasses counts and write the report
    auto counts = writer.get();
    counts.PoorSNR += poorSNR;
    counts.TooFewPasses += tooFewPasses;
    const string reportFile(options.get(OptionNames::ReportFile));

    if (reportFile == "-")
    {
        WriteResultsReport(cout, counts);
    }
    else
    {
        fstream stream(reportFile, fstream::out);
        WriteResultsReport(stream, counts);
    }

    return 0;
}
