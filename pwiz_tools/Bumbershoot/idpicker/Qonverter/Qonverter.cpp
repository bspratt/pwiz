﻿//
// $Id$
//
// The contents of this file are subject to the Mozilla Public License
// Version 1.1 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// http://www.mozilla.org/MPL/
//
// Software distributed under the License is distributed on an "AS IS"
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
// License for the specific language governing rights and limitations
// under the License.
//
// The Original Code is the IDPicker project.
//
// The Initial Developer of the Original Code is Matt Chambers.
//
// Copyright 2010 Vanderbilt University
//
// Contributor(s): Surendra Dasari
//


#include "Qonverter.hpp"
#include "../freicore/pwiz_src/pwiz/utility/misc/Std.hpp"
#include "boost/tuple/tuple.hpp"

#pragma unmanaged

#define CHECK_SQLITE_RESULT(x) \
    { /* anonymous scope to prevent name conflicts */ \
        char* errorBuf = NULL; \
        int rc = (x); \
        if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) \
        { \
            string error; \
            if (errorBuf) \
            { \
                error = errorBuf; \
                sqlite3_free(errorBuf); \
            } \
            throw runtime_error(error.c_str()); \
        } \
    }


//namespace {

const static string defaultDecoyPrefix = "rev_";

namespace DecoyState
{
    typedef char Type;
    const static Type Target = 'T';
    const static Type Decoy = 'D';
    const static Type Ambiguous = 'B';
}

struct sqlite3_wrapper
{
    sqlite3_wrapper(const string& filepath)
    {
        CHECK_SQLITE_RESULT(sqlite3_open(filepath.c_str(), &db));
    }

    ~sqlite3_wrapper()
    {
        CHECK_SQLITE_RESULT(sqlite3_close(db));
    }

    operator sqlite3* () {return db;}

    private:
    sqlite3* db;
};

typedef boost::tuple<string, string, string, string> AnalysisSourceChargeTuple;

struct PsmRow
{
    sqlite_int64 id;
    sqlite_int64 spectrum;
    DecoyState::Type decoyState;
    vector<double> scores;
    double totalScore;
};

struct SpectrumTopRank
{
    sqlite_int64 spectrum;
    double totalScore;
    DecoyState::Type decoyState;
    vector<size_t> psmIndexes;
    double qValue;
};

struct SpectrumTopRankGreaterThan
{
    bool operator() (const SpectrumTopRank& lhs, const SpectrumTopRank& rhs) const
    {
        return lhs.totalScore > rhs.totalScore;
    }
};

int getStringVector(void* data, int columnCount, char** columnValues, char** columnNames)
{
    if (columnCount != 1)
        throw runtime_error("[StaticWeightQonverter::getStringVector] result must have 1 column");

    static_cast<vector<string>*>(data)->push_back(columnValues[0]);

    return 0;
}

int getAnalysisSourceChargeTuples(void* data, int columnCount, char** columnValues, char** columnNames)
{
    if (columnCount != 5)
        throw runtime_error("[StaticWeightQonverter::getAnalysisSourceChargeTuples] result must have 5 columns");

    vector<AnalysisSourceChargeTuple>* tuples = static_cast<vector<AnalysisSourceChargeTuple>*>(data);
    tuples->push_back(boost::make_tuple(columnValues[0], columnValues[1], columnValues[2], columnValues[3]));

    return 0;
}

int getScoreNames(void* data, int columnCount, char** columnValues, char** columnNames)
{
    if (columnCount != 1)
        throw runtime_error("[StaticWeightQonverter::getScoreNames] result must have 1 column");

    if (columnValues[0] != NULL)
        bal::split(*static_cast<vector<string>*>(data), columnValues[0], bal::is_any_of(","));

    return 0;
}

int addPsmRow(void* data, int columnCount, char** columnValues, char** columnNames)
{
    if (columnCount != 4)
        throw runtime_error("[StaticWeightQonverter::addPsmRow] result must have 4 columns");

    PsmRow row;
    row.id = lexical_cast<sqlite_int64>(columnValues[0]);
    row.spectrum = lexical_cast<sqlite_int64>(columnValues[1]);

    switch (columnValues[2][0])
    {
        case '0': row.decoyState = DecoyState::Target; break;
        case '1': row.decoyState = DecoyState::Decoy; break;
        case '2': row.decoyState = DecoyState::Ambiguous; break;
    }

    vector<string> scores;
    bal::split(scores, columnValues[3], bal::is_any_of(","));
    row.scores.resize(scores.size());
    for (size_t i=0; i < scores.size(); ++i)
        row.scores[i] = lexical_cast<double>(scores[i]);

    static_cast<vector<PsmRow>*>(data)->push_back(row);
    return 0;
}

void qonvertPsmSubset(sqlite3* db, vector<PsmRow>& psmRows, const vector<double>& scoreWeights, bool logQonversionDetails)
{
    // calculate total scores for each PSM and keep track of the top ranked PSM(s) for each spectrum
    vector<SpectrumTopRank> spectrumTopRanks;
    for (size_t i=0; i < psmRows.size(); ++i)
    {
        PsmRow& row = psmRows[i];

        row.totalScore = 0;
        for (size_t j=0; j < scoreWeights.size(); ++j)
            row.totalScore += scoreWeights[j] * row.scores[j];

        // add a new SpectrumTopRank if this PSM comes from a different spectrum
        if (spectrumTopRanks.empty() || row.spectrum > spectrumTopRanks.back().spectrum)
        {
            spectrumTopRanks.push_back(SpectrumTopRank());
            SpectrumTopRank& topRank = spectrumTopRanks.back();
            topRank.spectrum = row.spectrum;
            topRank.decoyState = row.decoyState;
            topRank.totalScore = row.totalScore;
            topRank.psmIndexes.assign(1, i);
        }
        else
        {
            SpectrumTopRank& topRank = spectrumTopRanks.back();
            if (row.totalScore > topRank.totalScore)
            {
                // replace the top rank with this PSM
                topRank.totalScore = row.totalScore;
                topRank.decoyState = row.decoyState;
                topRank.psmIndexes.assign(1, i);
            }
            else if (row.totalScore == topRank.totalScore)
            {
                // add this PSM to the top rank
                if (topRank.decoyState != row.decoyState)
                    topRank.decoyState = DecoyState::Ambiguous;
                topRank.psmIndexes.push_back(i);
            }
        }
    }

    // sort the top ranks in descending order by total score
    sort(spectrumTopRanks.begin(), spectrumTopRanks.end(), SpectrumTopRankGreaterThan());

    int numTargets = 0;
    int numDecoys = 0;
    int numAmbiguous = 0;

    // calculate Q value for each PSM
    BOOST_FOREACH(SpectrumTopRank& topRank, spectrumTopRanks)
    {
        switch (topRank.decoyState)
        {
            case DecoyState::Target: ++numTargets; break;
            case DecoyState::Decoy: ++numDecoys; break;
            case DecoyState::Ambiguous: ++numAmbiguous; break;
        }

        if( numTargets + numDecoys > 0 )
		{
			topRank.qValue = double(numDecoys * 2 /*targetToDecoyRatio*/) /
                             double(numTargets + numDecoys);
        }
        else
            topRank.qValue = 0;
    }

    CHECK_SQLITE_RESULT(sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errorBuf));

    if (logQonversionDetails)
    {
        CHECK_SQLITE_RESULT(sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS TotalScores (PsmId, Spectrum, TotalScore, DecoyState)", NULL, NULL, &errorBuf));
        BOOST_FOREACH(const PsmRow& row, psmRows)
            CHECK_SQLITE_RESULT(sqlite3_exec(db, ("INSERT INTO TotalScores VALUES (" + \
                                                 lexical_cast<string>(row.id) + "," + \
                                                 lexical_cast<string>(row.spectrum) + "," + \
                                                 lexical_cast<string>(row.totalScore) + ",'" + \
                                                 lexical_cast<string>(row.decoyState) + "')").c_str(), NULL, NULL, &errorBuf));

        CHECK_SQLITE_RESULT(sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS QonversionDetails (Spectrum, TotalScore, DecoyState, QValue)", NULL, NULL, &errorBuf));
        BOOST_FOREACH(SpectrumTopRank& topRank, spectrumTopRanks)
            CHECK_SQLITE_RESULT(sqlite3_exec(db, ("INSERT INTO QonversionDetails VALUES (" + \
                                                 lexical_cast<string>(topRank.spectrum) + "," + \
                                                 lexical_cast<string>(topRank.totalScore) + ",'" + \
                                                 lexical_cast<string>(topRank.decoyState) + "'," + \
                                                 lexical_cast<string>(topRank.qValue) + ")").c_str(), NULL, NULL, &errorBuf));
    }

    // update QValue column for each top-ranked PSM (non-top-ranked PSMs keep the default QValue)
    string sql = "UPDATE PeptideSpectrumMatch SET QValue = ? WHERE Id = ?";

    sqlite3_stmt* updateStmt;
    CHECK_SQLITE_RESULT(sqlite3_prepare_v2(db, sql.c_str(), -1, &updateStmt, NULL));
    BOOST_FOREACH(SpectrumTopRank& topRank, spectrumTopRanks)
    {
        for (size_t i=0; i < topRank.psmIndexes.size(); ++i)
        {
            CHECK_SQLITE_RESULT(sqlite3_bind_double(updateStmt, 1, topRank.qValue));
            CHECK_SQLITE_RESULT(sqlite3_bind_int64(updateStmt, 2, psmRows[topRank.psmIndexes[i]].id));
            CHECK_SQLITE_RESULT(sqlite3_step(updateStmt));
            CHECK_SQLITE_RESULT(sqlite3_reset(updateStmt));
        }
    }
    CHECK_SQLITE_RESULT(sqlite3_finalize(updateStmt));

    CHECK_SQLITE_RESULT(sqlite3_exec(db, "COMMIT TRANSACTION", NULL, NULL, &errorBuf));
}

//} // namespace


namespace IDPicker {


#ifdef __cplusplus_cli
namespace native {
#endif


StaticWeightQonverter::StaticWeightQonverter()
{
    decoyPrefix = defaultDecoyPrefix;
    nTerminusIsSpecificWeight = 1;
    cTerminusIsSpecificWeight = 1;
    rerankMatches = false;
    logQonversionDetails = false;
}


void StaticWeightQonverter::Qonvert(const string& idpDbFilepath, const ProgressMonitor& progressMonitor)
{
    if (scoreWeights.empty())
        throw runtime_error("[StaticWeightQonverter::Qonvert] no score weights set");

    sqlite3_wrapper db(idpDbFilepath);

    sqlite3_exec(db, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF", NULL, NULL, NULL);

    Qonvert(db, progressMonitor);
}

void StaticWeightQonverter::Qonvert(sqlite3* db, const ProgressMonitor& progressMonitor)
{
    string sql;

    // get the set of distinct analysis/source/charge/specificity tuples
    sql = string("CREATE TEMP TABLE MostSpecificInstance (Peptide INTEGER PRIMARY KEY, Specificity INT);") +
                 "INSERT INTO MostSpecificInstance SELECT Peptide, MAX(NTerminusIsSpecific+CTerminusIsSpecific) FROM PeptideInstance GROUP BY Peptide;" +
                 "SELECT psm.Analysis, s.Source, psm.Charge, mostSpecificInstance.Specificity, COUNT(DISTINCT psm.Id) " +
                 "FROM PeptideSpectrumMatch psm " +
                 "JOIN MostSpecificInstance mostSpecificInstance ON psm.Peptide=mostSpecificInstance.Peptide " +
                 "JOIN Spectrum s ON psm.Spectrum=s.Id " +
                 "WHERE psm.QValue > 1 " +
                 (rerankMatches ? "" : "AND Rank = 1 ") +
                 "GROUP BY psm.Analysis, s.Source, psm.Charge, mostSpecificInstance.Specificity";

    vector<AnalysisSourceChargeTuple> analysisSourceChargeTuples;
    CHECK_SQLITE_RESULT(sqlite3_exec(db, sql.c_str(), getAnalysisSourceChargeTuples, &analysisSourceChargeTuples, &errorBuf));

    // get the set of expected score names
    set<string> expectedScoreNames;
    typedef pair<string, double> ScoreWeightPair;
    BOOST_FOREACH(const ScoreWeightPair& itr, scoreWeights)
        expectedScoreNames.insert(itr.first);

    // send initial progress update to indicate how many qonversion steps there are
    ProgressMonitor::UpdateMessage updateMessage;
    updateMessage.QonvertedAnalyses = 0;
    updateMessage.TotalAnalyses = analysisSourceChargeTuples.size();
    updateMessage.Cancel = false;
    progressMonitor(updateMessage);
    if (updateMessage.Cancel)
        return;

    // qonvert each analysis/source/charge tuple independently
    BOOST_FOREACH(const AnalysisSourceChargeTuple& analysisSourceChargeTuple, analysisSourceChargeTuples)
    {
        const string& analysisId = analysisSourceChargeTuple.get<0>();
        const string& spectrumSourceId = analysisSourceChargeTuple.get<1>();
        const string& psmChargeState = analysisSourceChargeTuple.get<2>();
        const string& specificity = analysisSourceChargeTuple.get<3>();

        // get the set of actual score ids and names
        sql = string("SELECT GROUP_CONCAT(scoreNames.Name || ' ' || scoreNames.Id) ") +
                     "FROM PeptideSpectrumMatchScores psmScore " +
                     "JOIN PeptideSpectrumMatchScoreNames scoreNames ON ScoreNameId=scoreNames.Id " +
                     "JOIN (SELECT psm.Id " +
                     "      FROM PeptideSpectrumMatch psm " +
                     "      JOIN Spectrum s ON Spectrum=s.Id " +
                     "      WHERE s.Source=" + spectrumSourceId +
                     "        AND psm.Analysis=" + analysisId +
                     (rerankMatches ? "" : " AND Rank = 1 ") +
                     "      LIMIT 1) AS psm ON psmScore.PsmId=psm.Id";

        vector<string> actualScoreIdNamePairs; // the order of scores is important!
        vector<string> actualScoreNames;
        map<string, string> actualScoreIdByName;

        CHECK_SQLITE_RESULT(sqlite3_exec(db, sql.c_str(), getScoreNames, &actualScoreIdNamePairs, &errorBuf));

        BOOST_FOREACH(const string& idName, actualScoreIdNamePairs)
        {
            vector<string> idNamePair;
            bal::split(idNamePair, idName, bal::is_space());
            actualScoreNames.push_back(idNamePair[0]);
            actualScoreIdByName[idNamePair[0]] = idNamePair[1];
        }

        if (actualScoreNames.empty())
        {
            ++updateMessage.QonvertedAnalyses;
            progressMonitor(updateMessage);
            if (updateMessage.Cancel)
                return;
            continue;
        }

        // the intersection between the sets is used as a SQL condition
        vector<string> scoreNameIntersection;
        set_intersection(actualScoreNames.begin(), actualScoreNames.end(),
                         expectedScoreNames.begin(), expectedScoreNames.end(),
                         std::back_inserter(scoreNameIntersection));

        if (scoreNameIntersection.empty())
            throw runtime_error("[StaticWeightQonverter::Qonvert] expected scores do not exist in the PSM set");

        // get the set of weights to use for calculating each PSM's total score;
        // if an actual score is not mapped in scoreWeights, it gets the default value of 0
        vector<double> scoreWeightsVector;
        vector<string> scoreIdSet;
        BOOST_FOREACH(const string& name, scoreNameIntersection)
        {
            scoreWeightsVector.push_back(scoreWeights[name]);
            scoreIdSet.push_back(actualScoreIdByName[name]);
        }

        // e.g. (1,2)
        string scoreIdSetString = "(" + bal::join(scoreIdSet, ",") + ")";

        // retrieve triplets of psm id, decoy state, and score list (with the same order retrieved above)
        sql = string("SELECT psm.Id, psm.Spectrum, ") +
                     "      CASE WHEN SUM(DISTINCT CASE WHEN pro.Accession LIKE '" + decoyPrefix + "%' THEN 1 ELSE 0 END) + SUM(DISTINCT CASE WHEN pro.Accession NOT LIKE '" + decoyPrefix + "%' THEN 1 ELSE 0 END) = 2 THEN 2 " +
                     "           ELSE SUM(DISTINCT CASE WHEN pro.Accession LIKE '" + decoyPrefix + "%' THEN 1 ELSE 0 END) " +
                     "           END AS DecoyState, " +
                     "      GROUP_CONCAT(psmScore.Value) " + // scores are ordered by ascending ScoreNameId
                     "FROM Spectrum s " +
                     "JOIN PeptideInstance pi ON psm.Peptide=pi.Peptide " +
                     "JOIN PeptideSpectrumMatch psm ON s.Id=psm.Spectrum " +
                     "JOIN PeptideSpectrumMatchScores psmScore ON psm.Id=psmScore.PsmId " +
                     "JOIN Protein pro ON pi.Protein=pro.Id " +
                     "WHERE s.Source=" + spectrumSourceId +
                     "  AND psm.Analysis=" + analysisId +
                     "  AND psm.Charge=" + psmChargeState +
                     "  AND NTerminusIsSpecific+CTerminusIsSpecific=" + specificity +
                     (rerankMatches ? "" : " AND Rank = 1 ") +
                     "  AND psmScore.ScoreNameId IN " + scoreIdSetString + " " +
                     "GROUP BY psm.Id "
                     "ORDER BY psm.Spectrum";

        vector<PsmRow> psmRows;
        CHECK_SQLITE_RESULT(sqlite3_exec(db, sql.c_str(), addPsmRow, &psmRows, &errorBuf));

        qonvertPsmSubset(db, psmRows, scoreWeightsVector, logQonversionDetails);

        ++updateMessage.QonvertedAnalyses;
        progressMonitor(updateMessage);
        if (updateMessage.Cancel)
            return;
    }
}


#ifdef __cplusplus_cli
} // namespace native
#endif


} // namespace IDPicker


#ifdef __cplusplus_cli

namespace IDPicker {


using namespace System::Runtime::InteropServices;

typedef void (__stdcall *QonversionProgressCallback)(int, int, bool&);


struct ProgressMonitorForwarder : public native::StaticWeightQonverter::ProgressMonitor
{
    QonversionProgressCallback managedFunctionPtr;

    ProgressMonitorForwarder(void* managedFunctionPtr)
        : managedFunctionPtr(static_cast<QonversionProgressCallback>(managedFunctionPtr))
    {}

    virtual void operator() (UpdateMessage& updateMessage) const
    {
        if (managedFunctionPtr != NULL)
        {
            managedFunctionPtr(updateMessage.QonvertedAnalyses,
                               updateMessage.TotalAnalyses,
                               updateMessage.Cancel);
        }
    }
};

#pragma managed
private delegate void QonversionProgressEventWrapper(int qonvertedAnalyses, int totalAnalyses, bool& cancel);

void StaticWeightQonverter::marshal(int qonvertedAnalyses, int totalAnalyses, bool& cancel)
{
    try
    {
        QonversionProgressEventArgs^ eventArgs = gcnew QonversionProgressEventArgs();
        eventArgs->QonvertedAnalyses = qonvertedAnalyses;
        eventArgs->TotalAnalyses = totalAnalyses;
        eventArgs->Cancel = cancel;
        QonversionProgress(this, eventArgs);
        cancel = eventArgs->Cancel;
    }
    catch (Exception^ e)
    {
        throw runtime_error(ToStdString(e->Message));
    }
}

StaticWeightQonverter::StaticWeightQonverter()
{
    ScoreWeights = gcnew Dictionary<String^, double>();

    IDPicker::native::StaticWeightQonverter swq;
    DecoyPrefix = ToSystemString(swq.decoyPrefix);
    NTerminusIsSpecificWeight = swq.nTerminusIsSpecificWeight;
    CTerminusIsSpecificWeight = swq.cTerminusIsSpecificWeight;
    RerankMatches = swq.rerankMatches;
    LogQonversionDetails = swq.logQonversionDetails;
}

void StaticWeightQonverter::Qonvert(String^ idpDbFilepath)
{
    IDPicker::native::StaticWeightQonverter swq;
    swq.decoyPrefix = ToStdString(DecoyPrefix);
    swq.nTerminusIsSpecificWeight = NTerminusIsSpecificWeight;
    swq.cTerminusIsSpecificWeight = CTerminusIsSpecificWeight;
    swq.rerankMatches = RerankMatches;
    swq.logQonversionDetails = LogQonversionDetails;

    for each (KeyValuePair<String^, double> itr in ScoreWeights)
        swq.scoreWeights[ToStdString(itr.Key)] = itr.Value;

    //if (!ReferenceEquals(%*QonversionProgress, nullptr))
    {
        QonversionProgressEventWrapper^ handler = gcnew QonversionProgressEventWrapper(this,
                                                    &StaticWeightQonverter::marshal);
        ProgressMonitorForwarder* progressMonitor = new ProgressMonitorForwarder(
            Marshal::GetFunctionPointerForDelegate(handler).ToPointer());

        try {swq.Qonvert(ToStdString(idpDbFilepath), *progressMonitor);} CATCH_AND_FORWARD
        delete progressMonitor;

        GC::KeepAlive(handler);
    }
    //else
    //    try {swq.Qonvert(ToStdString(idpDbFilepath));} CATCH_AND_FORWARD
}

void StaticWeightQonverter::Qonvert(System::IntPtr idpDb)
{
    IDPicker::native::StaticWeightQonverter swq;
    swq.decoyPrefix = ToStdString(DecoyPrefix);
    swq.nTerminusIsSpecificWeight = NTerminusIsSpecificWeight;
    swq.cTerminusIsSpecificWeight = CTerminusIsSpecificWeight;
    swq.rerankMatches = RerankMatches;
    swq.logQonversionDetails = LogQonversionDetails;

    for each (KeyValuePair<String^, double> itr in ScoreWeights)
        swq.scoreWeights[ToStdString(itr.Key)] = itr.Value;

    //if (!ReferenceEquals(%*QonversionProgress, nullptr))
    {
        QonversionProgressEventWrapper^ handler = gcnew QonversionProgressEventWrapper(this,
                                                    &StaticWeightQonverter::marshal);
        ProgressMonitorForwarder* progressMonitor = new ProgressMonitorForwarder(
            Marshal::GetFunctionPointerForDelegate(handler).ToPointer());

        sqlite3* foo = (sqlite3*) idpDb.ToPointer();
        pin_ptr<sqlite3> idpDbPtr = foo;
        try {swq.Qonvert(idpDbPtr, *progressMonitor);} CATCH_AND_FORWARD
        delete progressMonitor;

        GC::KeepAlive(handler);
    }
    //else
    //    try {swq.Qonvert(ToStdString(idpDbFilepath));} CATCH_AND_FORWARD
}


} // namespace IDPicker

#endif // __CLR__