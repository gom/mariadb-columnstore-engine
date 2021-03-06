/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

//  $Id: tupleaggregatestep.cpp 9732 2013-08-02 15:56:15Z pleblanc $


//#define NDEBUG
#include <cassert>
#include <sstream>
#include <iomanip>
#include <algorithm>
using namespace std;

#include <boost/shared_ptr.hpp>
#include <boost/shared_array.hpp>
#include <boost/scoped_array.hpp>
#include <boost/uuid/uuid_io.hpp>
using namespace boost;

#include "messagequeue.h"
using namespace messageqcpp;

#include "loggingid.h"
#include "errorcodes.h"
#include "idberrorinfo.h"
using namespace logging;

#include "configcpp.h"
using namespace config;

#include "calpontsystemcatalog.h"
#include "aggregatecolumn.h"
#include "arithmeticcolumn.h"
#include "functioncolumn.h"
#include "constantcolumn.h"
using namespace execplan;

#include "rowgroup.h"
#include "rowaggregation.h"
using namespace rowgroup;

#include "querytele.h"
using namespace querytele;

#include "jlf_common.h"
#include "jobstep.h"
#include "primitivestep.h"
#include "subquerystep.h"
#include "tuplehashjoin.h"
#include "crossenginestep.h"
#include "tupleaggregatestep.h"

//#include "stopwatch.cpp"

//Stopwatch timer;

namespace
{


typedef vector<Row::Pointer> RowBucket;
typedef vector<RowBucket> RowBucketVec;


inline RowAggFunctionType functionIdMap(int planFuncId)
{
	switch (planFuncId)
	{
		case AggregateColumn::COUNT_ASTERISK:           return ROWAGG_COUNT_ASTERISK;
		case AggregateColumn::COUNT:                    return ROWAGG_COUNT_COL_NAME;
		case AggregateColumn::SUM:                      return ROWAGG_SUM;
		case AggregateColumn::AVG:                      return ROWAGG_AVG;
		case AggregateColumn::MIN:                      return ROWAGG_MIN;
		case AggregateColumn::MAX:                      return ROWAGG_MAX;
		case AggregateColumn::DISTINCT_COUNT:           return ROWAGG_COUNT_DISTINCT_COL_NAME;
		case AggregateColumn::DISTINCT_SUM:             return ROWAGG_DISTINCT_SUM;
		case AggregateColumn::DISTINCT_AVG:             return ROWAGG_DISTINCT_AVG;
		case AggregateColumn::STDDEV_POP:               return ROWAGG_STATS;
		case AggregateColumn::STDDEV_SAMP:              return ROWAGG_STATS;
		case AggregateColumn::VAR_POP:                  return ROWAGG_STATS;
		case AggregateColumn::VAR_SAMP:                 return ROWAGG_STATS;
		case AggregateColumn::BIT_AND:                  return ROWAGG_BIT_AND;
		case AggregateColumn::BIT_OR:                   return ROWAGG_BIT_OR;
		case AggregateColumn::BIT_XOR:                  return ROWAGG_BIT_XOR;
		case AggregateColumn::GROUP_CONCAT:             return ROWAGG_GROUP_CONCAT;
		case AggregateColumn::CONSTANT:                 return ROWAGG_CONSTANT;
		default:                                        return ROWAGG_FUNCT_UNDEFINE;
	}
}


inline RowAggFunctionType statsFuncIdMap(int planFuncId)
{
	switch (planFuncId)
	{
		case AggregateColumn::STDDEV_POP:               return ROWAGG_STDDEV_POP;
		case AggregateColumn::STDDEV_SAMP:              return ROWAGG_STDDEV_SAMP;
		case AggregateColumn::VAR_POP:                  return ROWAGG_VAR_POP;
		case AggregateColumn::VAR_SAMP:                 return ROWAGG_VAR_SAMP;
		default:                                        return ROWAGG_FUNCT_UNDEFINE;
	}
}


inline string colTypeIdString(CalpontSystemCatalog::ColDataType type)
{
	switch (type)
	{
		case CalpontSystemCatalog::BIT:       return string("BIT");
		case CalpontSystemCatalog::TINYINT:   return string("TINYINT");
		case CalpontSystemCatalog::CHAR:      return string("CHAR");
		case CalpontSystemCatalog::SMALLINT:  return string("SMALLINT");
		case CalpontSystemCatalog::DECIMAL:   return string("DECIMAL");
		case CalpontSystemCatalog::MEDINT:    return string("MEDINT");
		case CalpontSystemCatalog::INT:       return string("INT");
		case CalpontSystemCatalog::FLOAT:     return string("FLOAT");
		case CalpontSystemCatalog::DATE:      return string("DATE");
		case CalpontSystemCatalog::BIGINT:    return string("BIGINT");
		case CalpontSystemCatalog::DOUBLE:    return string("DOUBLE");
		case CalpontSystemCatalog::DATETIME:  return string("DATETIME");
		case CalpontSystemCatalog::VARCHAR:   return string("VARCHAR");
		case CalpontSystemCatalog::CLOB:      return string("CLOB");
		case CalpontSystemCatalog::BLOB:      return string("BLOB");
		case CalpontSystemCatalog::UTINYINT:  return string("UTINYINT");
		case CalpontSystemCatalog::USMALLINT: return string("USMALLINT");
		case CalpontSystemCatalog::UDECIMAL:  return string("UDECIMAL");
		case CalpontSystemCatalog::UMEDINT:   return string("UMEDINT");
		case CalpontSystemCatalog::UINT:      return string("UINT");
		case CalpontSystemCatalog::UFLOAT:    return string("UFLOAT");
		case CalpontSystemCatalog::UBIGINT:   return string("UBIGINT");
		case CalpontSystemCatalog::UDOUBLE:   return string("UDOUBLE");
		default:                              return string("UNKNOWN");
	}
}


string keyName(uint64_t i, uint32_t key, const joblist::JobInfo& jobInfo)
{
	string name = jobInfo.projectionCols[i]->alias();
	if (name.empty())
	{
		name = jobInfo.keyInfo->tupleKeyToName[key];
		if (jobInfo.keyInfo->tupleKeyVec[key].fId < 100)
			name = "Expression/Function";
	}

	return name = "'" + name + "'";
}


}


namespace joblist
{


TupleAggregateStep::TupleAggregateStep(
	const SP_ROWAGG_UM_t& agg,
	const RowGroup& rgOut,
	const RowGroup& rgIn,
	const JobInfo& jobInfo) :
		JobStep(jobInfo),
		fCatalog(jobInfo.csc),
		fRowsReturned(0),
		fDoneAggregate(false),
		fEndOfResult(false),
		fAggregator(agg),
		fRowGroupOut(rgOut),
		fRowGroupIn(rgIn),
		fUmOnly(false),
		fRm(jobInfo.rm),
		fBucketNum(0),
		fInputIter(-1),
		fSessionMemLimit(jobInfo.umMemLimit)
{
	fRowGroupData.reinit(fRowGroupOut);
	fRowGroupOut.setData(&fRowGroupData);
	fAggregator->setInputOutput(fRowGroupIn, &fRowGroupOut);

	// decide if this needs to be multi-threaded
	RowAggregationDistinct* multiAgg = dynamic_cast<RowAggregationDistinct*>(fAggregator.get());
	fIsMultiThread = (multiAgg || fAggregator->aggMapKeyLength() > 0);

	// initialize multi-thread variables
	fNumOfThreads = fRm.aggNumThreads();
	fNumOfBuckets = fRm.aggNumBuckets();
	fNumOfRowGroups = fRm.aggNumRowGroups();
	fMemUsage.reset(new uint64_t[fNumOfThreads]);
	memset(fMemUsage.get(), 0, fNumOfThreads * sizeof(uint64_t));

	fExtendedInfo = "TAS: ";
	fQtc.stepParms().stepType = StepTeleStats::T_TAS;
}


TupleAggregateStep::~TupleAggregateStep()
{
	for (uint32_t i = 0; i < fNumOfThreads; i++)
		fRm.returnMemory(fMemUsage[i], fSessionMemLimit);
	for (uint32_t i = 0; i < fAgg_mutex.size(); i++)
		delete fAgg_mutex[i];
}


void TupleAggregateStep::initializeMultiThread()
{
	RowGroupDL *dlIn = fInputJobStepAssociation.outAt(0)->rowGroupDL();
    uint32_t i;

	if (dlIn == NULL)
		throw logic_error("Input is not RowGroup data list in delivery step.");

	if (fInputIter < 0)
		fInputIter = dlIn->getIterator();

	fRowGroupIns.resize(fNumOfThreads);
	fRowGroupOuts.resize(fNumOfBuckets);
	fRowGroupDatas.resize(fNumOfBuckets);

	rowgroup::SP_ROWAGG_UM_t agg;
	RGData rgData;

	for (i = 0; i < fNumOfBuckets; i++)
	{
		mutex *lock = new mutex();
		fAgg_mutex.push_back(lock);
		fRowGroupOuts[i] = fRowGroupOut;
		rgData.reinit(fRowGroupOut);
		fRowGroupDatas[i] = rgData;
		fRowGroupOuts[i].setData(&fRowGroupDatas[i]);
		fRowGroupOuts[i].resetRowGroup(0);
	}
}


void TupleAggregateStep::run()
{
	if (fDelivery == false)
	{
		fRunner.reset(new thread(Aggregator(this)));
	}
}


void TupleAggregateStep::join()
{
	if (fRunner)
		fRunner->join();
}


void TupleAggregateStep::doThreadedSecondPhaseAggregate(uint32_t threadID)
{
    if (threadID >= fNumOfBuckets)
        return;

	scoped_array<RowBucketVec> rowBucketVecs(new RowBucketVec[fNumOfBuckets]);
	scoped_array<bool> bucketDone(new bool[fNumOfBuckets]);
	uint32_t hashlen = fAggregator->aggMapKeyLength();
	bool caughtException = false;

	try
	{
		RowAggregationDistinct *aggDist = dynamic_cast<RowAggregationDistinct*>(fAggregators[threadID].get());
		RowAggregationMultiDistinct *multiDist = dynamic_cast<RowAggregationMultiDistinct*>(fAggregators[threadID].get());
		Row rowIn;
		RowGroup* rowGroupIn = 0;
		rowGroupIn = (aggDist->aggregator()->getOutputRowGroup());
		uint32_t bucketID;

		if (multiDist)
		{
			for (uint32_t i = 0; i < fNumOfBuckets; i++)
				rowBucketVecs[i].resize(multiDist->subAggregators().size());
		}
		else
		{
			for (uint32_t i = 0; i < fNumOfBuckets; i++)
				rowBucketVecs[i].resize(1);
		}

		// dispatch rows to bucket
		if (multiDist)
		{
			for (uint32_t j = 0; j < multiDist->subAggregators().size(); j++)
			{
				rowGroupIn = (multiDist->subAggregators()[j]->getOutputRowGroup());
				rowGroupIn->initRow(&rowIn);
				while (dynamic_cast<RowAggregationUM*>(multiDist->subAggregators()[j].get())->nextRowGroup())
				{
					rowGroupIn = (multiDist->subAggregators()[j]->getOutputRowGroup());
					rowGroupIn->getRow(0, &rowIn);
					for (uint64_t i = 0; i < rowGroupIn->getRowCount(); ++i)
					{
						// The key is the groupby columns, which are the leading columns.
						//uint8_t* hashMapKey = rowIn.getData() + 2;
						//bucketID = hash.operator()(hashMapKey) & fBucketMask;
						bucketID = rowIn.hash(hashlen-1) % fNumOfBuckets;
						rowBucketVecs[bucketID][j].push_back(rowIn.getPointer());
						rowIn.nextRow();
					}
				}
			}
		}
		else
		{
			rowGroupIn->initRow(&rowIn);
			while (dynamic_cast<RowAggregationUM*>(aggDist->aggregator().get())->nextRowGroup())
			{
				rowGroupIn->setData(aggDist->aggregator()->getOutputRowGroup()->getRGData());
				rowGroupIn->getRow(0, &rowIn);
				for (uint64_t i = 0; i < rowGroupIn->getRowCount(); ++i)
				{
					// The key is the groupby columns, which are the leading columns.
					//uint8_t* hashMapKey = rowIn.getData() + 2;
					//bucketID = hash.operator()(hashMapKey) & fBucketMask;
					bucketID = rowIn.hash(hashlen-1) % fNumOfBuckets;
					rowBucketVecs[bucketID][0].push_back(rowIn.getPointer());
					rowIn.nextRow();
				}
			}
		}

		bool done = false;

		// reset bucketDone[] to be false
		//memset(bucketDone, 0, sizeof(bucketDone));
		fill(&bucketDone[0], &bucketDone[fNumOfBuckets], false);
		while (!done && !cancelled())
		{
			done = true;
			for (uint32_t c = 0; c < fNumOfBuckets && !cancelled(); c++)
			{
				if (!bucketDone[c] && fAgg_mutex[c]->try_lock())
				{
					try
					{
						if (multiDist)
							dynamic_cast<RowAggregationMultiDistinct*>(fAggregators[c].get())->doDistinctAggregation_rowVec(rowBucketVecs[c]);
						else
							dynamic_cast<RowAggregationDistinct*>(fAggregators[c].get())->doDistinctAggregation_rowVec(rowBucketVecs[c][0]);
					}
					catch(...)
					{
						fAgg_mutex[c]->unlock();
						throw;
					}
					fAgg_mutex[c]->unlock();
					bucketDone[c] = true;
					rowBucketVecs[c][0].clear();
				}
				else if (!bucketDone[c])
				{
					done = false;
				}
			}
		}

		if (cancelled())
		{
			fEndOfResult = true;
		}

	} // try
	catch (IDBExcept& iex)
	{
		catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
			(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
		caughtException = true;
	}
	catch(const std::exception& ex)
	{
		catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);
		caughtException = true;
	}
	catch(...)
	{
		catchHandler("doThreadedSecondPhaseAggregate() caught an unknown exception",
						tupleAggregateStepErr, fErrorInfo, fSessionId);
		caughtException = true;
	}

	if (caughtException)
		fEndOfResult = true;

	fDoneAggregate = true;

	if (traceOn())
	{
		dlTimes.setLastReadTime();
		dlTimes.setEndOfInputTime();
	}
}


uint32_t TupleAggregateStep::nextBand_singleThread(messageqcpp::ByteStream &bs)
{
	uint32_t rowCount = 0;

	try
	{
		if (!fDoneAggregate)
			aggregateRowGroups();

		if (fEndOfResult == false)
		{
			bs.restart();
			// do the final aggregtion and deliver the results
			// at least one RowGroup for aggregate results
			if (dynamic_cast<RowAggregationDistinct*>(fAggregator.get()) != NULL)
			{
				dynamic_cast<RowAggregationDistinct*>(fAggregator.get())->doDistinctAggregation();
			}

			if (fAggregator->nextRowGroup())
			{
				fAggregator->finalize();
				rowCount = fRowGroupOut.getRowCount();
				fRowsReturned += rowCount;
				fRowGroupDelivered.setData(fRowGroupOut.getRGData());
				if (fRowGroupOut.getColumnCount() != fRowGroupDelivered.getColumnCount())
					pruneAuxColumns();
				fRowGroupDelivered.serializeRGData(bs);
			}
			else
			{
				fEndOfResult = true;
			}
		}
	} // try
	catch (IDBExcept& iex)
	{
		catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
			(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));

		fEndOfResult = true;
	}
	catch(const std::exception& ex)
	{
		catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);

		fEndOfResult = true;
	}
	catch(...)
	{
		catchHandler("TupleAggregateStep next band caught an unknown exception",
						tupleAggregateStepErr, fErrorInfo, fSessionId);

		fEndOfResult = true;
	}

	if (fEndOfResult)
	{
		StepTeleStats sts;
		sts.query_uuid = fQueryUuid;
		sts.step_uuid = fStepUuid;
		sts.msg_type = StepTeleStats::ST_SUMMARY;
		sts.total_units_of_work = sts.units_of_work_completed = 1;
		sts.rows = fRowsReturned;
		postStepSummaryTele(sts);

		// send an empty / error band
		RGData rgData(fRowGroupOut, 0);
		fRowGroupOut.setData(&rgData);
		fRowGroupOut.resetRowGroup(0);
		fRowGroupOut.setStatus(status());
		fRowGroupOut.serializeRGData(bs);
		rowCount = 0;

		if (traceOn())
			printCalTrace();
	}

	return rowCount;
}


bool TupleAggregateStep::nextDeliveredRowGroup()
{
	for (; fBucketNum < fNumOfBuckets; fBucketNum++)
	{
		while (fAggregators[fBucketNum]->nextRowGroup())
		{
			fAggregators[fBucketNum]->finalize();
			fRowGroupDelivered.setData(fAggregators[fBucketNum]->getOutputRowGroup()->getRGData());
			fRowGroupOut.setData(fAggregators[fBucketNum]->getOutputRowGroup()->getRGData());
			return true;
		}
	}
	fBucketNum = 0;
	return false;
}


uint32_t TupleAggregateStep::nextBand(messageqcpp::ByteStream &bs)
{
	// use the orignal single thread model when no group by and distnct.
	// @bug4314. DO NOT access fAggregtor before the first read of input,
	// because hashjoin may not have finalized fAggregator.
	if (!fIsMultiThread)
		return nextBand_singleThread(bs);

	return doThreadedAggregate(bs, 0);
}


bool TupleAggregateStep::setPmHJAggregation(JobStep* step)
{
	TupleBPS* bps = dynamic_cast<TupleBPS*>(step);
	if (bps != NULL)
	{
		fAggregatorUM->expression(fAggregator->expression());
		fAggregatorUM->constantAggregate(fAggregator->constantAggregate());
		fAggregator = fAggregatorUM;
		fRowGroupIn = fRowGroupPMHJ;
		fAggregator->setInputOutput(fRowGroupIn, &fRowGroupOut);
		bps->setAggregateStep(fAggregatorPM, fRowGroupPMHJ);
	}

	return (bps != NULL);
}


void TupleAggregateStep::configDeliveredRowGroup(const JobInfo& jobInfo)
{
	// configure the oids and keys
	vector<uint32_t> oids = fRowGroupOut.getOIDs();
	vector<uint32_t> keys = fRowGroupOut.getKeys();
	vector<pair<int, int> >::const_iterator begin = jobInfo.aggEidIndexList.begin();
	vector<pair<int, int> >::const_iterator end   = jobInfo.aggEidIndexList.end();
	for (vector<pair<int, int> >::const_iterator i = begin; i != end; i++)
	{
		oids[i->second] = i->first;
		keys[i->second] = getExpTupleKey(jobInfo, i->first);
	}

	// correct the scale
	vector<uint32_t> scale = fRowGroupOut.getScale();
	for (uint64_t i = 0; i < scale.size(); i++)
	{
		// to support CNX_DECIMAL_SCALE the avg column's scale is coded with two scales:
		// fe's avg column scale << 8 + original column scale
		//if ((scale[i] & 0x0000FF00) > 0)
		scale[i] = scale[i] &  0x000000FF;
	}

	size_t retColCount = jobInfo.nonConstDelCols.size();
	if (jobInfo.havingStep)
		retColCount = jobInfo.returnedColVec.size();
	vector<uint32_t>::const_iterator offsets0 = fRowGroupOut.getOffsets().begin();
	vector<CalpontSystemCatalog::ColDataType>::const_iterator types0 =
																fRowGroupOut.getColTypes().begin();

	vector<uint32_t>::const_iterator precision0 = fRowGroupOut.getPrecision().begin();
	fRowGroupDelivered = RowGroup(retColCount,
							vector<uint32_t>(offsets0, offsets0+retColCount+1),
							vector<uint32_t>(oids.begin(), oids.begin()+retColCount),
							vector<uint32_t>(keys.begin(), keys.begin()+retColCount),
							vector<CalpontSystemCatalog::ColDataType>(types0, types0+retColCount),
							vector<uint32_t>(scale.begin(), scale.begin()+retColCount),
							vector<uint32_t>(precision0, precision0+retColCount),
							jobInfo.stringTableThreshold);

	if (jobInfo.trace)
		cout << "delivered RG: " << fRowGroupDelivered.toString() << endl << endl;

}


void TupleAggregateStep::setOutputRowGroup(const RowGroup& rg)
{
	fRowGroupOut = rg;
	fRowGroupData.reinit(fRowGroupOut);
	fRowGroupOut.setData(&fRowGroupData);
	fAggregator->setInputOutput(fRowGroupIn, &fRowGroupOut);
}


const RowGroup& TupleAggregateStep::getOutputRowGroup() const
{
	return fRowGroupOut;
}


const RowGroup& TupleAggregateStep::getDeliveredRowGroup() const
{
	return fRowGroupDelivered;
}


void TupleAggregateStep::savePmHJData(SP_ROWAGG_t& um, SP_ROWAGG_t& pm, RowGroup& rg)
{
	fAggregatorUM = dynamic_pointer_cast<RowAggregationUM>(um);
	fAggregatorPM = pm;
	fRowGroupPMHJ = rg;
}


void TupleAggregateStep::deliverStringTableRowGroup(bool b)
{
	fRowGroupDelivered.setUseStringTable(b);
}


bool TupleAggregateStep::deliverStringTableRowGroup() const
{
	return fRowGroupDelivered.usesStringTable();
}


const string TupleAggregateStep::toString() const
{
	ostringstream oss;
	oss << "AggregateStep   ses:" << fSessionId << " txn:" << fTxnId << " st:" << fStepId;

	oss << " in:";
	for (unsigned i = 0; i < fInputJobStepAssociation.outSize(); i++)
		oss << fInputJobStepAssociation.outAt(i);

	if (fOutputJobStepAssociation.outSize() > 0)
	{
		oss << " out:";
		for (unsigned i = 0; i < fOutputJobStepAssociation.outSize(); i++)
			oss << fOutputJobStepAssociation.outAt(i);
	}

	return oss.str();
}


SJSTEP TupleAggregateStep::prepAggregate(SJSTEP& step, JobInfo& jobInfo)
{
	SJSTEP spjs;
	TupleBPS* tbps = dynamic_cast<TupleBPS*>(step.get());
	TupleHashJoinStep* thjs = dynamic_cast<TupleHashJoinStep*>(step.get());
	SubAdapterStep* sas = dynamic_cast<SubAdapterStep*>(step.get());
	CrossEngineStep* ces = dynamic_cast<CrossEngineStep*>(step.get());
	vector<RowGroup> rgs;      // 0-ProjRG, 1-UMRG, [2-PMRG -- if 2 phases]
	vector<SP_ROWAGG_t> aggs;
	SP_ROWAGG_UM_t aggUM;
	bool distinctAgg = false;
	int64_t constKey = -1;
	vector<ConstantAggData> constAggDataVec;

	vector<std::pair<uint32_t, int> > returnedColVecOrig = jobInfo.returnedColVec;
	for(uint32_t idx = 0; idx < jobInfo.returnedColVec.size(); idx++)
	{
		if (jobInfo.returnedColVec[idx].second == AggregateColumn::DISTINCT_COUNT ||
			jobInfo.returnedColVec[idx].second == AggregateColumn::DISTINCT_AVG ||
			jobInfo.returnedColVec[idx].second == AggregateColumn::DISTINCT_SUM
			)
		{
			distinctAgg = true;
		}

		// Change COUNT_ASTERISK to CONSTANT if necessary.
		// In joblistfactory, all aggregate(constant) are set to count(*) for easy process.
		map<uint64_t, SRCP>::iterator it = jobInfo.constAggregate.find(idx);
		if (it != jobInfo.constAggregate.end())
		{
			AggregateColumn* ac = dynamic_cast<AggregateColumn*>(it->second.get());
			if (ac->aggOp() == AggregateColumn::COUNT_ASTERISK)
			{
				if(jobInfo.cntStarPos == -1)
					jobInfo.cntStarPos = idx;
			}
			else
			{
				constKey = jobInfo.returnedColVec[idx].first;
				CalpontSystemCatalog::ColType ct = ac->resultType();
				TupleInfo ti =
					setExpTupleInfo(ct, ac->expressionId(), ac->alias(), jobInfo);
				jobInfo.returnedColVec[idx].first = ti.key;
				jobInfo.returnedColVec[idx].second = AggregateColumn::CONSTANT;

				ConstantColumn* cc = dynamic_cast<ConstantColumn*>(ac->constCol().get());
				idbassert(cc != NULL);   // @bug5261
				bool isNull = (ConstantColumn::NULLDATA == cc->type());
				constAggDataVec.push_back(
					ConstantAggData(cc->constval(), functionIdMap(ac->aggOp()), isNull));
			}
		}
	}

	// If there are aggregate(constant) columns, but no count(*), add a count(*).
	if (constAggDataVec.size() > 0 && jobInfo.cntStarPos < 0)
	{
		jobInfo.cntStarPos = jobInfo.returnedColVec.size();
		jobInfo.returnedColVec.push_back(make_pair(constKey, AggregateColumn::COUNT_ASTERISK));
	}

	// preprocess the columns used by group_concat
	jobInfo.groupConcatInfo.prepGroupConcat(jobInfo);
	bool doGroupConcat = false;

	if (tbps != NULL)
	{
		// get rowgroup and aggregator
		rgs.push_back(tbps->getDeliveredRowGroup());
		if (jobInfo.groupConcatInfo.columns().size() == 0)
		{
			if (distinctAgg == true)
				prep2PhasesDistinctAggregate(jobInfo, rgs, aggs);
			else
				prep2PhasesAggregate(jobInfo, rgs, aggs);
		}
		else
		{
			if (distinctAgg == true)
				prep1PhaseDistinctAggregate(jobInfo, rgs, aggs);
			else
				prep1PhaseAggregate(jobInfo, rgs, aggs);

			// TODO: fix this
			rgs.push_back(rgs[0]);
			doGroupConcat = true;
		}

		// make sure connected by a RowGroupDL
		JobStepAssociation tbpsJsa;
		AnyDataListSPtr spdl(new AnyDataList());
		RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
		dl->OID(execplan::CNX_VTABLE_ID);
		spdl->rowGroupDL(dl);
		tbpsJsa.outAdd(spdl);

		// create delivery step
		aggUM = dynamic_pointer_cast<RowAggregationUM>(aggs[0]);
		spjs.reset(new TupleAggregateStep(aggUM, rgs[1], rgs[2], jobInfo));
		spjs->inputAssociation(tbpsJsa);

		// step id??
		spjs->stepId(step->stepId()+1);

		// set the PM/UM side aggregate structs
		tbps->outputAssociation(tbpsJsa);
		if (doGroupConcat)
			dynamic_cast<TupleAggregateStep*>(spjs.get())->umOnly(true);
		else
			tbps->setAggregateStep(aggs[1], rgs[2]);
	}
	else if (thjs != NULL)
	{
		// default to UM aggregation
		rgs.push_back(thjs->getDeliveredRowGroup());
		if (distinctAgg == true)
			prep1PhaseDistinctAggregate(jobInfo, rgs, aggs);
		else
			prep1PhaseAggregate(jobInfo, rgs, aggs);

		// also prepare for PM aggregation
		// rowgroups   -- 0-proj, 1-um, [2-phase case: 2-um, 3-pm]
		// aggregators -- 0-um, [2-phase case: 1-um, 2-pm]
		if (jobInfo.groupConcatInfo.columns().size() == 0)
		{
			if (distinctAgg == true)
				prep2PhasesDistinctAggregate(jobInfo, rgs, aggs);
			else
				prep2PhasesAggregate(jobInfo, rgs, aggs);
		}
		else
		{
			// TODO: fix this
			rgs.push_back(rgs[0]);
			doGroupConcat = true;
		}

		// make sure connected by a RowGroupDL
		JobStepAssociation thjsJsa;
		AnyDataListSPtr spdl(new AnyDataList());
		RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
		dl->OID(execplan::CNX_VTABLE_ID);
		spdl->rowGroupDL(dl);
		thjsJsa.outAdd(spdl);

		// create delivery step
		aggUM = dynamic_pointer_cast<RowAggregationUM>(aggs[0]);
		spjs.reset(new TupleAggregateStep(aggUM, rgs[1], rgs[0], jobInfo));
		spjs->inputAssociation(thjsJsa);

		if (doGroupConcat)
			dynamic_cast<TupleAggregateStep*>(spjs.get())->umOnly(true);
		else
			dynamic_cast<TupleAggregateStep*>(spjs.get())->savePmHJData(aggs[1], aggs[2], rgs[3]);


		// step id??
		spjs->stepId(step->stepId()+1);

		// set input side
		thjs->outputAssociation(thjsJsa);
		thjs->deliveryStep(spjs);
	}
	else if (sas != NULL)
	{
		// UM aggregation
		// rowgroups   -- 0-proj, 1-um
		// aggregators -- 0-um
		rgs.push_back(sas->getDeliveredRowGroup());
		if (distinctAgg == true)
			prep1PhaseDistinctAggregate(jobInfo, rgs, aggs);
		else
			prep1PhaseAggregate(jobInfo, rgs, aggs);

		// make sure connected by a RowGroupDL
		JobStepAssociation sasJsa;
		AnyDataListSPtr spdl(new AnyDataList());
		RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
		dl->OID(execplan::CNX_VTABLE_ID);
		spdl->rowGroupDL(dl);
		sasJsa.outAdd(spdl);

		// create delivery step
		aggUM = dynamic_pointer_cast<RowAggregationUM>(aggs[0]);
		spjs.reset(new TupleAggregateStep(aggUM, rgs[1], rgs[0], jobInfo));
		spjs->inputAssociation(sasJsa);

		// step id??
		spjs->stepId(step->stepId()+1);

		// set input side
		sas->outputAssociation(sasJsa);
	}
	else if (ces != NULL)
	{
		// UM aggregation
		// rowgroups   -- 0-proj, 1-um
		// aggregators -- 0-um
		rgs.push_back(ces->getDeliveredRowGroup());
		if (distinctAgg == true)
			prep1PhaseDistinctAggregate(jobInfo, rgs, aggs);
		else
			prep1PhaseAggregate(jobInfo, rgs, aggs);

		// make sure connected by a RowGroupDL
		JobStepAssociation cesJsa;
		AnyDataListSPtr spdl(new AnyDataList());
		RowGroupDL* dl = new RowGroupDL(1, jobInfo.fifoSize);
		dl->OID(execplan::CNX_VTABLE_ID);
		spdl->rowGroupDL(dl);
		cesJsa.outAdd(spdl);

		// create delivery step
		aggUM = dynamic_pointer_cast<RowAggregationUM>(aggs[0]);
		spjs.reset(new TupleAggregateStep(aggUM, rgs[1], rgs[0], jobInfo));
		spjs->inputAssociation(cesJsa);

		// step id??
		spjs->stepId(step->stepId()+1);

		// set input side
		ces->outputAssociation(cesJsa);
	}

	// add the aggregate on constants
	if (constAggDataVec.size() > 0)
	{
		dynamic_cast<TupleAggregateStep*>(spjs.get())->addConstangAggregate(constAggDataVec);
		jobInfo.returnedColVec.swap(returnedColVecOrig); // restore the original return columns
	}

	// fix the delivered rowgroup data
	dynamic_cast<TupleAggregateStep*>(spjs.get())->configDeliveredRowGroup(jobInfo);

	if (jobInfo.expressionVec.size() > 0)
		dynamic_cast<TupleAggregateStep*>(spjs.get())->prepExpressionOnAggregate(aggUM, jobInfo);

	return spjs;
}


void TupleAggregateStep::prep1PhaseAggregate(
	JobInfo& jobInfo, vector<RowGroup>& rowgroups, vector<SP_ROWAGG_t>& aggregators)
{
	// check if there are any aggregate columns
	vector<pair<uint32_t, int> > aggColVec;
	vector<std::pair<uint32_t, int> >& returnedColVec = jobInfo.returnedColVec;
	for (uint64_t i = 0; i < returnedColVec.size(); i++)
	{
		if (returnedColVec[i].second != 0)
			aggColVec.push_back(returnedColVec[i]);
	}

	// populate the aggregate rowgroup: projectedRG   -> aggregateRG
	//
	// Aggregate preparation by joblist factory:
	// 1. get projected rowgroup (done by doAggProject) -- passed in
	// 2. construct aggregate rowgroup  -- output of UM
	const RowGroup projRG = rowgroups[0];
	const vector<uint32_t>& oidsProj = projRG.getOIDs();
	const vector<uint32_t>& keysProj = projRG.getKeys();
	const vector<uint32_t>& scaleProj = projRG.getScale();
	const vector<uint32_t>& precisionProj = projRG.getPrecision();
	const vector<CalpontSystemCatalog::ColDataType>& typeProj = projRG.getColTypes();

	vector<uint32_t> posAgg;
	vector<uint32_t> oidsAgg;
	vector<uint32_t> keysAgg;
	vector<uint32_t> scaleAgg;
	vector<uint32_t> precisionAgg;
	vector<CalpontSystemCatalog::ColDataType> typeAgg;
	vector<uint32_t> widthAgg;
	vector<SP_ROWAGG_GRPBY_t> groupBy;
	vector<SP_ROWAGG_FUNC_t> functionVec;
	uint32_t bigIntWidth = sizeof(int64_t);
	uint32_t bigUintWidth = sizeof(uint64_t);

	// for count column of average function
	map<uint32_t, SP_ROWAGG_FUNC_t> avgFuncMap;

	// collect the projected column info, prepare for aggregation
	vector<uint32_t> width;
	map<uint32_t, int> projColPosMap;
	for (uint64_t i = 0; i < keysProj.size(); i++)
	{
		projColPosMap.insert(make_pair(keysProj[i], i));
		width.push_back(projRG.getColumnWidth(i));
	}

	// for groupby column
	map<uint32_t, int> groupbyMap;
	for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
	{
		int64_t colProj = projColPosMap[jobInfo.groupByColVec[i]];
		SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, -1));
		groupBy.push_back(groupby);
		groupbyMap.insert(make_pair(jobInfo.groupByColVec[i], i));
	}

	// for distinct column
	for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
	{
		//@bug6126, continue if already in group by
		if (groupbyMap.find(jobInfo.distinctColVec[i]) != groupbyMap.end())
			continue;

		int64_t colProj = projColPosMap[jobInfo.distinctColVec[i]];
		SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, -1));
		groupBy.push_back(groupby);
		groupbyMap.insert(make_pair(jobInfo.distinctColVec[i], i));
	}

	// populate the aggregate rowgroup
	map<pair<uint32_t, int>, uint64_t> aggFuncMap;
	for (uint64_t i = 0; i < returnedColVec.size(); i++)
	{
		RowAggFunctionType aggOp = functionIdMap(returnedColVec[i].second);
		RowAggFunctionType stats = statsFuncIdMap(returnedColVec[i].second);
		uint32_t key = returnedColVec[i].first;

		if (aggOp == ROWAGG_CONSTANT)
		{
			TupleInfo ti = getTupleInfo(key, jobInfo);
			oidsAgg.push_back(ti.oid);
			keysAgg.push_back(key);
			scaleAgg.push_back(ti.scale);
			precisionAgg.push_back(ti.precision);
			typeAgg.push_back(ti.dtype);
			widthAgg.push_back(ti.width);
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
					aggOp, stats, 0, i, jobInfo.cntStarPos));
			functionVec.push_back(funct);
			continue;
		}

		if (aggOp == ROWAGG_GROUP_CONCAT)
		{
			TupleInfo ti = getTupleInfo(key, jobInfo);
			uint32_t ptrSize = sizeof(GroupConcatAg*);
			uint32_t width = (ti.width >= ptrSize) ? ti.width : ptrSize;
			oidsAgg.push_back(ti.oid);
			keysAgg.push_back(key);
			scaleAgg.push_back(ti.scale);
			precisionAgg.push_back(ti.precision);
			typeAgg.push_back(ti.dtype);
			widthAgg.push_back(width);
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
					aggOp, stats, 0, i, -1));
			functionVec.push_back(funct);

			continue;
		}

		if (projColPosMap.find(key) == projColPosMap.end())
		{
			ostringstream emsg;
			emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
			cerr << "prep1PhaseAggregate: " << emsg.str()
				 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
				 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
			if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
				 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
			cerr << endl;
			throw logic_error(emsg.str());
		}

		// make sure the colProj is correct
		int64_t colProj = projColPosMap[key];
		if (keysProj[colProj] != key)
		{
			ostringstream emsg;
			emsg << "projection column map is out of sync.";
			cerr << "prep1PhaseAggregate: " << emsg.str() << endl;
			throw logic_error(emsg.str());
		}

		if (aggOp == ROWAGG_FUNCT_UNDEFINE)
		{
			// must be a groupby column or function on aggregation
			// or used by group_concat
			map<uint32_t, int>::iterator it = groupbyMap.find(key);
			if (it != groupbyMap.end())
			{
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(scaleProj[colProj]);
				precisionAgg.push_back(precisionProj[colProj]);
				typeAgg.push_back(typeProj[colProj]);
				widthAgg.push_back(width[colProj]);
				if (groupBy[it->second]->fOutputColumnIndex == (uint32_t) -1)
					groupBy[it->second]->fOutputColumnIndex = i;
				else
					functionVec.push_back(SP_ROWAGG_FUNC_t(
						new RowAggFunctionCol(
							ROWAGG_DUP_FUNCT,
							ROWAGG_FUNCT_UNDEFINE,
							-1,
							i,
							groupBy[it->second]->fOutputColumnIndex)));
				continue;
			}
			else if (find(jobInfo.expressionVec.begin(), jobInfo.expressionVec.end(), key) !=
				jobInfo.expressionVec.end())
			{
				TupleInfo ti = getTupleInfo(key, jobInfo);
				oidsAgg.push_back(ti.oid);
				keysAgg.push_back(key);
				scaleAgg.push_back(ti.scale);
				precisionAgg.push_back(ti.precision);
				typeAgg.push_back(ti.dtype);
				widthAgg.push_back(ti.width);
				continue;
			}
			else if (jobInfo.groupConcatInfo.columns().find(key) !=
						jobInfo.groupConcatInfo.columns().end())
			{
				// TODO: columns only for group_concat do not needed in result set.
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(scaleProj[colProj]);
				precisionAgg.push_back(precisionProj[colProj]);
				typeAgg.push_back(typeProj[colProj]);
				widthAgg.push_back(width[colProj]);
				continue;
			}
			else if (jobInfo.windowSet.find(key) != jobInfo.windowSet.end())
			{
				// skip window columns/expression, which are computed later
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(scaleProj[colProj]);
				precisionAgg.push_back(precisionProj[colProj]);
				typeAgg.push_back(typeProj[colProj]);
				widthAgg.push_back(width[colProj]);
				continue;
			}
			else
			{
				Message::Args args;
				args.add(keyName(i, key, jobInfo));
				string emsg = IDBErrorInfo::instance()->errorMsg(ERR_NOT_GROUPBY_EXPRESSION, args);
				cerr << "prep1PhaseAggregate: " << emsg << " oid="
					 << (int) jobInfo.keyInfo->tupleKeyVec[key].fId << ", alias="
					 << jobInfo.keyInfo->tupleKeyVec[key].fTable << ", view="
					 << jobInfo.keyInfo->tupleKeyVec[key].fView << ", function="
					 << (int) aggOp << endl;
				throw IDBExcept(emsg, ERR_NOT_GROUPBY_EXPRESSION);
			}
		}

		SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colProj, i));
		functionVec.push_back(funct);

		switch (aggOp)
		{
			case ROWAGG_MIN:
			case ROWAGG_MAX:
			{
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(scaleProj[colProj]);
				precisionAgg.push_back(precisionProj[colProj]);
				typeAgg.push_back(typeProj[colProj]);
				widthAgg.push_back(width[colProj]);
			}
			break;

			case ROWAGG_AVG:
				avgFuncMap.insert(make_pair(key, funct));
			case ROWAGG_SUM:
			{
				if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
					typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
					typeProj[colProj] == CalpontSystemCatalog::DATE ||
					typeProj[colProj] == CalpontSystemCatalog::DATETIME)
				{
					Message::Args args;
					args.add("sum/average");
					args.add(colTypeIdString(typeProj[colProj]));
					string emsg =
						IDBErrorInfo::instance()->errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
					cerr << "prep1PhaseAggregate: " << emsg << endl;
					throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
				}

				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				if (typeProj[colProj] == CalpontSystemCatalog::DOUBLE ||
					typeProj[colProj] == CalpontSystemCatalog::UDOUBLE ||
					typeProj[colProj] == CalpontSystemCatalog::FLOAT ||
					typeProj[colProj] == CalpontSystemCatalog::UFLOAT)
				{
					typeAgg.push_back(typeProj[colProj]);
					scaleAgg.push_back(scaleProj[colProj]);
					precisionAgg.push_back(precisionProj[colProj]);
					widthAgg.push_back(width[colProj]);
				}
				else if (isUnsigned(typeProj[colProj]))
				{
					typeAgg.push_back(CalpontSystemCatalog::UBIGINT);
					uint32_t scale = scaleProj[colProj];
					// for int average, FE expects a decimal
					if (aggOp == ROWAGG_AVG)
						scale = jobInfo.scaleOfAvg[key]; // scale += 4;
					scaleAgg.push_back(scale);
					precisionAgg.push_back(20);
					widthAgg.push_back(bigUintWidth);
				}
				else
				{
					typeAgg.push_back(CalpontSystemCatalog::BIGINT);
					uint32_t scale = scaleProj[colProj];
					// for int average, FE expects a decimal
					if (aggOp == ROWAGG_AVG)
						scale = jobInfo.scaleOfAvg[key]; // scale += 4;
					scaleAgg.push_back(scale);
					precisionAgg.push_back(19);
					widthAgg.push_back(bigIntWidth);
				}
			}
			break;

			case ROWAGG_COUNT_COL_NAME:
			case ROWAGG_COUNT_ASTERISK:
			{
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(0);
				// work around count() in select subquery
				precisionAgg.push_back(9999);
				typeAgg.push_back(CalpontSystemCatalog::BIGINT);
				widthAgg.push_back(bigIntWidth);
			}
			break;

			case ROWAGG_STATS:
			{
				if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
					typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
					typeProj[colProj] == CalpontSystemCatalog::DATE ||
					typeProj[colProj] == CalpontSystemCatalog::DATETIME)
				{
					Message::Args args;
					args.add("variance/standard deviation");
					args.add(colTypeIdString(typeProj[colProj]));
					string emsg =
						IDBErrorInfo::instance()->errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
					cerr << "prep1PhaseAggregate: " << emsg << endl;
					throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
				}

				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(scaleProj[colProj]);
				precisionAgg.push_back(0);
				typeAgg.push_back(CalpontSystemCatalog::DOUBLE);
				widthAgg.push_back(sizeof(double));
			}
			break;

			case ROWAGG_BIT_AND:
			case ROWAGG_BIT_OR:
			case ROWAGG_BIT_XOR:
			{
				oidsAgg.push_back(oidsProj[colProj]);
				keysAgg.push_back(key);
				scaleAgg.push_back(0);
				precisionAgg.push_back(-16);  // for connector to skip null check
				typeAgg.push_back(CalpontSystemCatalog::BIGINT);
				widthAgg.push_back(bigIntWidth);
			}
			break;

			default:
			{
				ostringstream emsg;
				emsg << "aggregate function (" << (uint64_t) aggOp << ") isn't supported";
				cerr << "prep1PhaseAggregate: " << emsg.str() << endl;
				throw QueryDataExcept(emsg.str(), aggregateFuncErr);
			}
		}

		// find if this func is a duplicate
		map<pair<uint32_t, int>, uint64_t>::iterator iter =
			aggFuncMap.find(make_pair(key, aggOp));
		if (iter != aggFuncMap.end())
		{
			if (funct->fAggFunction == ROWAGG_AVG)
				funct->fAggFunction = ROWAGG_DUP_AVG;
			else if (funct->fAggFunction == ROWAGG_STATS)
				funct->fAggFunction = ROWAGG_DUP_STATS;
			else
				funct->fAggFunction = ROWAGG_DUP_FUNCT;

			funct->fAuxColumnIndex = iter->second;
		}
		else
		{
			aggFuncMap.insert(make_pair(make_pair(key, aggOp), funct->fOutputColumnIndex));
		}
	}

	// now fix the AVG function, locate the count(column) position
	for (uint64_t i = 0; i < functionVec.size(); i++)
	{
		if (functionVec[i]->fAggFunction != ROWAGG_COUNT_COL_NAME)
			continue;

		// if the count(k) can be associated with an avg(k)
		map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
			avgFuncMap.find(keysAgg[functionVec[i]->fOutputColumnIndex]);
		if (k != avgFuncMap.end())
		{
			k->second->fAuxColumnIndex = functionVec[i]->fOutputColumnIndex;
			functionVec[i]->fAggFunction = ROWAGG_COUNT_NO_OP;
		}
	}

	// there is avg(k), but no count(k) in the select list
	uint64_t lastCol = returnedColVec.size();
	for (map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k=avgFuncMap.begin(); k!=avgFuncMap.end(); k++)
	{
		if (k->second->fAuxColumnIndex == (uint32_t) -1)
		{
			k->second->fAuxColumnIndex = lastCol++;
			oidsAgg.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
			keysAgg.push_back(k->first);
			scaleAgg.push_back(0);
			precisionAgg.push_back(19);
			typeAgg.push_back(CalpontSystemCatalog::BIGINT);
			widthAgg.push_back(bigIntWidth);
		}
	}

	// add auxiliary fields for statistics functions
	for (uint64_t i = 0; i < functionVec.size(); i++)
	{
		if (functionVec[i]->fAggFunction != ROWAGG_STATS)
			continue;

		functionVec[i]->fAuxColumnIndex = lastCol;
		uint64_t j = functionVec[i]->fInputColumnIndex;

		// sum(x)
		oidsAgg.push_back(oidsAgg[j]);
		keysAgg.push_back(keysAgg[j]);
		scaleAgg.push_back(0);
		precisionAgg.push_back(0);
		typeAgg.push_back(CalpontSystemCatalog::LONGDOUBLE);
		widthAgg.push_back(sizeof(long double));
		++lastCol;

		// sum(x**2)
		oidsAgg.push_back(oidsAgg[j]);
		keysAgg.push_back(keysAgg[j]);
		scaleAgg.push_back(0);
		precisionAgg.push_back(0);
		typeAgg.push_back(CalpontSystemCatalog::LONGDOUBLE);
		widthAgg.push_back(sizeof(long double));
		++lastCol;
	}

	// calculate the offset and create the rowaggregation, rowgroup
	posAgg.push_back(2);
	for (uint64_t i = 0; i < oidsAgg.size(); i++)
		posAgg.push_back(posAgg[i] + widthAgg[i]);
	RowGroup aggRG(oidsAgg.size(), posAgg, oidsAgg, keysAgg, typeAgg, scaleAgg, precisionAgg,
		jobInfo.stringTableThreshold);
	SP_ROWAGG_UM_t rowAgg(new RowAggregationUM(groupBy, functionVec, &jobInfo.rm, jobInfo.umMemLimit));
	rowgroups.push_back(aggRG);
	aggregators.push_back(rowAgg);

	// mapping the group_concat columns, if any.
	if (jobInfo.groupConcatInfo.groupConcat().size() > 0)
	{
		jobInfo.groupConcatInfo.mapColumns(projRG);
		rowAgg->groupConcat(jobInfo.groupConcatInfo.groupConcat());
	}

	if (jobInfo.trace)
		cout << "\n====== Aggregation RowGroups ======" << endl
			 << "projected  RG: " << projRG.toString() << endl
			 << "aggregated RG: " << aggRG.toString() << endl;
}


void TupleAggregateStep::prep1PhaseDistinctAggregate(
							JobInfo& jobInfo,
							vector<RowGroup>& rowgroups,
							vector<SP_ROWAGG_t>& aggregators)
{
	// check if there are any aggregate columns
	vector<pair<uint32_t, int> > aggColVec;
	vector<std::pair<uint32_t, int> >& returnedColVec = jobInfo.returnedColVec;
	for (uint64_t i = 0; i < returnedColVec.size(); i++)
	{
		if (returnedColVec[i].second != 0)
			aggColVec.push_back(returnedColVec[i]);
	}

	// populate the aggregate rowgroup: projectedRG   -> aggregateRG
	//
	// Aggregate preparation by joblist factory:
	// 1. get projected rowgroup (done by doAggProject) -- passed in
	// 2. construct aggregate rowgroup  -- output of UM
	const RowGroup projRG = rowgroups[0];
	const vector<uint32_t>& oidsProj = projRG.getOIDs();
	const vector<uint32_t>& keysProj = projRG.getKeys();
	const vector<uint32_t>& scaleProj = projRG.getScale();
	const vector<uint32_t>& precisionProj = projRG.getPrecision();
	const vector<CalpontSystemCatalog::ColDataType>& typeProj = projRG.getColTypes();

	vector<uint32_t> posAgg, posAggDist;
	vector<uint32_t> oidsAgg, oidsAggDist;
	vector<uint32_t> keysAgg, keysAggDist;
	vector<uint32_t> scaleAgg, scaleAggDist;
	vector<uint32_t> precisionAgg, precisionAggDist;
	vector<CalpontSystemCatalog::ColDataType> typeAgg, typeAggDist;
	vector<uint32_t> widthProj, widthAgg, widthAggDist;
	vector<SP_ROWAGG_GRPBY_t> groupBy, groupByNoDist;
	vector<SP_ROWAGG_FUNC_t> functionVec1, functionVec2, functionNoDistVec;
	uint32_t bigIntWidth = sizeof(int64_t);
	map<pair<uint32_t, int>, uint64_t> aggFuncMap;
	set<uint32_t> avgSet;

	// for count column of average function
	map<uint32_t, SP_ROWAGG_FUNC_t> avgFuncMap, avgDistFuncMap;

	// associate the columns between projected RG and aggregate RG on UM
	// populated the aggregate columns
	//     the groupby columns are put in front, even not a returned column
	//     sum and count(column name) are omitted, if avg present
	{
		// project only uniq oids, but they may be repeated in aggregation
		// collect the projected column info, prepare for aggregation
		map<uint32_t, int> projColPosMap;
		for (uint64_t i = 0; i < keysProj.size(); i++)
		{
			projColPosMap.insert(make_pair(keysProj[i], i));
			widthProj.push_back(projRG.getColumnWidth(i));
		}

		// column index for aggregate rowgroup
		uint64_t colAgg = 0;

		// for groupby column
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			uint32_t key = jobInfo.groupByColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep1PhaseDistinctAggregate: groupby " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			uint64_t colProj = projColPosMap[key];

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAgg));
			groupBy.push_back(groupby);

			// copy down to aggregation rowgroup
			oidsAgg.push_back(oidsProj[colProj]);
			keysAgg.push_back(key);
			scaleAgg.push_back(scaleProj[colProj]);
			precisionAgg.push_back(precisionProj[colProj]);
			typeAgg.push_back(typeProj[colProj]);
			widthAgg.push_back(widthProj[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAgg[colAgg], 0), colAgg));
			colAgg++;
		}

		// for distinct column
		for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
		{
			uint32_t key = jobInfo.distinctColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep1PhaseDistinctAggregate: distinct " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			// check for dup distinct column -- @bug6126
			if (find(keysAgg.begin(), keysAgg.end(), key) != keysAgg.end())
				continue;

			uint64_t colProj = projColPosMap[key];

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAgg));
			groupBy.push_back(groupby);

			// copy down to aggregation rowgroup
			oidsAgg.push_back(oidsProj[colProj]);
			keysAgg.push_back(key);
			scaleAgg.push_back(scaleProj[colProj]);
			precisionAgg.push_back(precisionProj[colProj]);
			typeAgg.push_back(typeProj[colProj]);
			widthAgg.push_back(widthProj[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAgg[colAgg], 0), colAgg));
			colAgg++;
		}

		// vectors for aggregate functions
		for (uint64_t i = 0; i < aggColVec.size(); i++)
		{
			uint32_t aggKey = aggColVec[i].first;
			RowAggFunctionType aggOp = functionIdMap(aggColVec[i].second);
			RowAggFunctionType stats = statsFuncIdMap(aggColVec[i].second);

			// skip if this is a constant
			if (aggOp == ROWAGG_CONSTANT)
				continue;

			// skip if this is a group_concat
			if (aggOp == ROWAGG_GROUP_CONCAT)
			{
				TupleInfo ti = getTupleInfo(aggKey, jobInfo);
				uint32_t width = sizeof(GroupConcatAg*);
				oidsAgg.push_back(ti.oid);
				keysAgg.push_back(aggKey);
				scaleAgg.push_back(ti.scale);
				precisionAgg.push_back(ti.precision);
				typeAgg.push_back(ti.dtype);
				widthAgg.push_back(width);
				SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
						aggOp, stats, colAgg, colAgg, -1));
				functionVec1.push_back(funct);
				aggFuncMap.insert(make_pair(make_pair(aggKey, aggOp), colAgg));
				colAgg++;

				continue;
			}

			if (projColPosMap.find(aggKey) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[aggKey] << "' isn't in tuple.";
				cerr << "prep1PhaseDistinctAggregate: aggregate " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[aggKey].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[aggKey].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			// skip sum / count(column) if avg is also selected
			if ((aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME) &&
				(avgSet.find(aggKey) != avgSet.end()))
				continue;

			// skip if this is a duplicate
			if (aggFuncMap.find(make_pair(aggKey, aggOp)) != aggFuncMap.end())
				continue;

			if (aggOp == ROWAGG_DISTINCT_SUM ||
				aggOp == ROWAGG_DISTINCT_AVG ||
				aggOp == ROWAGG_COUNT_DISTINCT_COL_NAME)
				continue;

			uint64_t colProj = projColPosMap[aggKey];
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colProj, colAgg));
			functionVec1.push_back(funct);

			aggFuncMap.insert(make_pair(make_pair(aggKey, aggOp), colAgg));

			switch (aggOp)
			{
				case ROWAGG_MIN:
				case ROWAGG_MAX:
				{
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(scaleProj[colProj]);
					precisionAgg.push_back(precisionProj[colProj]);
					typeAgg.push_back(typeProj[colProj]);
					widthAgg.push_back(widthProj[colProj]);
					colAgg++;
				}
				break;

				case ROWAGG_SUM:
				case ROWAGG_AVG:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("sum/average");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep1PhaseDistinctAggregate: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					if (typeProj[colProj] != CalpontSystemCatalog::DOUBLE &&
						typeProj[colProj] != CalpontSystemCatalog::FLOAT)
					{
						typeAgg.push_back(CalpontSystemCatalog::BIGINT);
						uint32_t scale = scaleProj[colProj];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_AVG)
							scale = jobInfo.scaleOfAvg[aggKey]; // scale += 4;
						scaleAgg.push_back(scale);
						precisionAgg.push_back(19);
						widthAgg.push_back(bigIntWidth);
					}
					else
					{
						typeAgg.push_back(typeProj[colProj]);
						scaleAgg.push_back(scaleProj[colProj]);
						precisionAgg.push_back(precisionProj[colProj]);
						widthAgg.push_back(widthProj[colProj]);
					}
					colAgg++;
				}
				// has distinct step, put the count column for avg next to the sum
				// let fall through to add a count column for average function
				if (aggOp == ROWAGG_AVG)
					funct->fAuxColumnIndex = colAgg;
				else
					break;

				case ROWAGG_COUNT_ASTERISK:
				case ROWAGG_COUNT_COL_NAME:
				{
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(0);
					// work around count() in select subquery
					precisionAgg.push_back(9999);
					typeAgg.push_back(CalpontSystemCatalog::BIGINT);
					widthAgg.push_back(bigIntWidth);
					colAgg++;
				}
				break;

				case ROWAGG_STATS:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("variance/standard deviation");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep1PhaseDistinctAggregate:: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					// count(x)
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(scaleProj[colProj]);
					precisionAgg.push_back(0);
					typeAgg.push_back(CalpontSystemCatalog::DOUBLE);
					widthAgg.push_back(sizeof(double));
					funct->fAuxColumnIndex = ++colAgg;

					// sum(x)
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(0);
					precisionAgg.push_back(0);
					typeAgg.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAgg.push_back(sizeof(long double));
					++colAgg;

					// sum(x**2)
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(0);
					precisionAgg.push_back(0);
					typeAgg.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAgg.push_back(sizeof(long double));
					++colAgg;
				}
				break;

				case ROWAGG_BIT_AND:
				case ROWAGG_BIT_OR:
				case ROWAGG_BIT_XOR:
				{
					oidsAgg.push_back(oidsProj[colProj]);
					keysAgg.push_back(aggKey);
					scaleAgg.push_back(0);
					precisionAgg.push_back(-16);  // for connector to skip null check
					typeAgg.push_back(CalpontSystemCatalog::BIGINT);
					widthAgg.push_back(bigIntWidth);
					colAgg++;
				}
				break;

				default:
				{
					ostringstream emsg;
					emsg << "aggregate function (" << (uint64_t) aggOp << ") isn't supported";
					cerr << "prep1PhaseDistinctAggregate: " << emsg.str() << endl;
					throw QueryDataExcept(emsg.str(), aggregateFuncErr);
				}
			}
		}
	}

	// populated the functionNoDistVec
	{
//		for (uint32_t idx = 0; idx < functionVec1.size(); idx++)
//		{
//			SP_ROWAGG_FUNC_t func1 = functionVec1[idx];
//			SP_ROWAGG_FUNC_t funct(
//					new RowAggFunctionCol(func1->fAggFunction,
//					func1->fStatsFunction,
//					func1->fOutputColumnIndex,
//					func1->fOutputColumnIndex,
//					func1->fAuxColumnIndex));
//			functionNoDistVec.push_back(funct);
//		}
		functionNoDistVec = functionVec1;
	}

	// associate the columns between the non-distinct aggregator and distinct aggregator
	// populated the returned columns
	//     remove not returned groupby column
	//     add back sum or count(column name) if omitted due to avg column
	//     put count(column name) column to the end, if it is for avg only
	{
		// check if the count column for AVG is also a returned column,
		// if so, replace the "-1" to actual position in returned vec.
		map<pair<uint32_t, RowAggFunctionType>, uint64_t> aggDupFuncMap;

		// copy over the groupby vector
		// update the outputColumnIndex if returned
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(i, -1));
			groupByNoDist.push_back(groupby);
			aggFuncMap.insert(make_pair(make_pair(keysAgg[i], 0), i));
		}

		// locate the return column position in aggregated rowgroup
		for (uint64_t i = 0; i < returnedColVec.size(); i++)
		{
			uint32_t retKey = returnedColVec[i].first;
			RowAggFunctionType aggOp = functionIdMap(returnedColVec[i].second);
			RowAggFunctionType stats = statsFuncIdMap(returnedColVec[i].second);
			int colAgg = -1;
			if  (find(jobInfo.distinctColVec.begin(), jobInfo.distinctColVec.end(), retKey) !=
					jobInfo.distinctColVec.end() )
			{
				map<pair<uint32_t, int>, uint64_t>::iterator it = aggFuncMap.find(make_pair(retKey, 0));
				if (it != aggFuncMap.end())
				{
					colAgg = it->second;
				}
				else
				{
					ostringstream emsg;
					emsg << "'" << jobInfo.keyInfo->tupleKeyToName[retKey] << "' isn't in tuple.";
					cerr << "prep1PhaseDistinctAggregate: distinct " << emsg.str()
						 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[retKey].fId
						 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[retKey].fTable;
					if (jobInfo.keyInfo->tupleKeyVec[retKey].fView.length() > 0)
						 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[retKey].fView;
					cerr << endl;
					throw QueryDataExcept(emsg.str(), aggregateFuncErr);
				}
			}

			switch (aggOp)
			{
				case ROWAGG_DISTINCT_AVG:
				case ROWAGG_DISTINCT_SUM:
				{
					if (typeAgg[colAgg] == CalpontSystemCatalog::CHAR ||
						typeAgg[colAgg] == CalpontSystemCatalog::VARCHAR ||
						typeAgg[colAgg] == CalpontSystemCatalog::DATE ||
						typeAgg[colAgg] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("sum/average");
						args.add(colTypeIdString(typeAgg[colAgg]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep1PhaseDistinctAggregate: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					oidsAggDist.push_back(oidsAgg[colAgg]);
					keysAggDist.push_back(retKey);
					if (typeAgg[colAgg] != CalpontSystemCatalog::DOUBLE &&
						typeAgg[colAgg] != CalpontSystemCatalog::FLOAT)
					{
						typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
						uint32_t scale = scaleProj[colAgg];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_DISTINCT_AVG)
							scale = jobInfo.scaleOfAvg[retKey]; // scale += 4;
						scaleAggDist.push_back(scale);
						precisionAggDist.push_back(19);
						widthAggDist.push_back(bigIntWidth);
					}
					else
					{
						typeAggDist.push_back(typeAgg[colAgg]);
						scaleAggDist.push_back(scaleAgg[colAgg]);
						precisionAggDist.push_back(precisionAgg[colAgg]);
						widthAggDist.push_back(widthAgg[colAgg]);
					}
				}
				break;

				case ROWAGG_COUNT_DISTINCT_COL_NAME:
				{
					oidsAggDist.push_back(oidsAgg[colAgg]);
					keysAggDist.push_back(retKey);
					scaleAggDist.push_back(0);
					// work around count() in select subquery
					precisionAggDist.push_back(9999);
					typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
					widthAggDist.push_back(bigIntWidth);
				}
				break;

				case ROWAGG_MIN:
				case ROWAGG_MAX:
				case ROWAGG_SUM:
				case ROWAGG_AVG:
				case ROWAGG_COUNT_ASTERISK:
				case ROWAGG_COUNT_COL_NAME:
				case ROWAGG_STATS:
				case ROWAGG_BIT_AND:
				case ROWAGG_BIT_OR:
				case ROWAGG_BIT_XOR:
				default:
				{
					map<pair<uint32_t, int>, uint64_t>::iterator it =
						aggFuncMap.find(make_pair(retKey, aggOp));
					if (it != aggFuncMap.end())
					{
						colAgg = it->second;
						oidsAggDist.push_back(oidsAgg[colAgg]);
						keysAggDist.push_back(keysAgg[colAgg]);
						scaleAggDist.push_back(scaleAgg[colAgg]);
						precisionAggDist.push_back(precisionAgg[colAgg]);
						typeAggDist.push_back(typeAgg[colAgg]);
						uint32_t width = widthAgg[colAgg];
						if (aggOp == ROWAGG_GROUP_CONCAT)
						{
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							if (ti.width > width)
								width = ti.width;
						}
						widthAggDist.push_back(width);
					}

					// not a direct hit -- a returned column is not already in the RG from PMs
					else
					{
						bool returnColMissing = true;

						// check if a SUM or COUNT covered by AVG
						if (aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME)
						{
							it = aggFuncMap.find(make_pair(returnedColVec[i].first, ROWAGG_AVG));
							if (it != aggFuncMap.end())
							{
								// false alarm
								returnColMissing = false;

								colAgg = it->second;
								if (aggOp == ROWAGG_SUM)
								{
									oidsAggDist.push_back(oidsAgg[colAgg]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(scaleAgg[colAgg] >> 8);
									precisionAggDist.push_back(precisionAgg[colAgg]);
									typeAggDist.push_back(typeAgg[colAgg]);
									widthAggDist.push_back(widthAgg[colAgg]);
								}
								else
								{
									// leave the count() to avg
									aggOp = ROWAGG_COUNT_NO_OP;

									oidsAggDist.push_back(oidsAgg[colAgg]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(0);
									precisionAggDist.push_back(19);
									typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
									widthAggDist.push_back(bigIntWidth);
								}
							}
						}
						else if (find(jobInfo.expressionVec.begin(), jobInfo.expressionVec.end(),
								 retKey) != jobInfo.expressionVec.end())
						{
							// a function on aggregation
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}
						else if (aggOp == ROWAGG_CONSTANT)
						{
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}
#if 0
						else if (aggOp == ROWAGG_GROUP_CONCAT)
						{
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}
#endif
						else if (jobInfo.groupConcatInfo.columns().find(retKey) !=
								jobInfo.groupConcatInfo.columns().end())
						{
							// TODO: columns only for group_concat do not needed in result set.
							for (uint64_t k = 0; k < keysProj.size(); k++)
							{
								if (retKey == keysProj[k])
								{
									oidsAggDist.push_back(oidsProj[k]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(scaleProj[k] >> 8);
									precisionAggDist.push_back(precisionProj[k]);
									typeAggDist.push_back(typeProj[k]);
									widthAggDist.push_back(widthProj[k]);

									returnColMissing = false;
									break;
								}
							}
						}

						else if (jobInfo.windowSet.find(retKey) != jobInfo.windowSet.end())
						{
							// skip window columns/expression, which are computed later
							for (uint64_t k = 0; k < keysProj.size(); k++)
							{
								if (retKey == keysProj[k])
								{
									oidsAggDist.push_back(oidsProj[k]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(scaleProj[k] >> 8);
									precisionAggDist.push_back(precisionProj[k]);
									typeAggDist.push_back(typeProj[k]);
									widthAggDist.push_back(widthProj[k]);

									returnColMissing = false;
									break;
								}
							}
						}

						if (returnColMissing)
						{
							Message::Args args;
							args.add(keyName(i, retKey, jobInfo));
							string emsg = IDBErrorInfo::instance()->
												errorMsg(ERR_NOT_GROUPBY_EXPRESSION, args);
							cerr << "prep1PhaseDistinctAggregate: " << emsg << " oid="
								 << (int) jobInfo.keyInfo->tupleKeyVec[retKey].fId << ", alias="
								 << jobInfo.keyInfo->tupleKeyVec[retKey].fTable << ", view="
								 << jobInfo.keyInfo->tupleKeyVec[retKey].fView << ", function="
								 << (int) aggOp << endl;
							throw IDBExcept(emsg, ERR_NOT_GROUPBY_EXPRESSION);
						}
					} //else
				} // switch
			}

			// update groupby vector if the groupby column is a returned column
			if (returnedColVec[i].second == 0)
			{
				int dupGroupbyIndex = -1;
				for (uint64_t j = 0; j < jobInfo.groupByColVec.size(); j++)
				{
					if (jobInfo.groupByColVec[j] == retKey)
					{
						if (groupByNoDist[j]->fOutputColumnIndex == (uint32_t) -1)
							groupByNoDist[j]->fOutputColumnIndex = i;
						else
							dupGroupbyIndex = groupByNoDist[j]->fOutputColumnIndex;
					}
				}

				// a duplicate group by column
				if (dupGroupbyIndex != -1)
					functionVec2.push_back(SP_ROWAGG_FUNC_t(
						new RowAggFunctionCol(
							ROWAGG_DUP_FUNCT, ROWAGG_FUNCT_UNDEFINE, -1, i, dupGroupbyIndex)));
			}

			// update the aggregate function vector
			else
			{
				SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colAgg, i));
				if (aggOp == ROWAGG_COUNT_NO_OP)
					funct->fAuxColumnIndex = colAgg;
				else if (aggOp == ROWAGG_CONSTANT)
					funct->fAuxColumnIndex = jobInfo.cntStarPos;

				functionVec2.push_back(funct);

				// find if this func is a duplicate
				map<pair<uint32_t, RowAggFunctionType>, uint64_t>::iterator iter =
					aggDupFuncMap.find(make_pair(retKey, aggOp));
				if (iter != aggDupFuncMap.end())
				{
					if (funct->fAggFunction == ROWAGG_AVG)
						funct->fAggFunction = ROWAGG_DUP_AVG;
					else if (funct->fAggFunction == ROWAGG_STATS)
						funct->fAggFunction = ROWAGG_DUP_STATS;
					else
						funct->fAggFunction = ROWAGG_DUP_FUNCT;
					funct->fAuxColumnIndex = iter->second;
				}
				else
				{
					aggDupFuncMap.insert(make_pair(make_pair(retKey, aggOp),
																funct->fOutputColumnIndex));
				}

				if (returnedColVec[i].second == AggregateColumn::AVG)
					avgFuncMap.insert(make_pair(returnedColVec[i].first, funct));
				else if (returnedColVec[i].second == AggregateColumn::DISTINCT_AVG)
					avgDistFuncMap.insert(make_pair(returnedColVec[i].first, funct));
			}
		} // for (i

		// now fix the AVG function, locate the count(column) position
		for (uint64_t i = 0; i < functionVec2.size(); i++)
		{
			if (functionVec2[i]->fAggFunction == ROWAGG_COUNT_NO_OP)
			{
				// if the count(k) can be associated with an avg(k)
				map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
					avgFuncMap.find(keysAggDist[functionVec2[i]->fOutputColumnIndex]);
				if (k != avgFuncMap.end())
					k->second->fAuxColumnIndex = functionVec2[i]->fOutputColumnIndex;
			}
		}

		// there is avg(k), but no count(k) in the select list
		uint64_t lastCol = returnedColVec.size();
		for (map<uint32_t,SP_ROWAGG_FUNC_t>::iterator k=avgFuncMap.begin();k!=avgFuncMap.end();k++)
		{
			if (k->second->fAuxColumnIndex == (uint32_t) -1)
			{
				k->second->fAuxColumnIndex = lastCol++;
				oidsAggDist.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
				keysAggDist.push_back(k->first);
				scaleAggDist.push_back(0);
				precisionAggDist.push_back(19);
				typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
				widthAggDist.push_back(bigIntWidth);
			}
		}

		// now fix the AVG distinct function, locate the count(distinct column) position
		for (uint64_t i = 0; i < functionVec2.size(); i++)
		{
			if (functionVec2[i]->fAggFunction == ROWAGG_COUNT_DISTINCT_COL_NAME)
			{
				// if the count(distinct k) can be associated with an avg(distinct k)
				map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
					avgDistFuncMap.find(keysAggDist[functionVec2[i]->fOutputColumnIndex]);
				if (k != avgDistFuncMap.end())
				{
					k->second->fAuxColumnIndex = functionVec2[i]->fOutputColumnIndex;
					functionVec2[i]->fAggFunction = ROWAGG_COUNT_NO_OP;
				}
			}
		}

		// there is avg(distinct k), but no count(distinct k) in the select list
		for (map<uint32_t,SP_ROWAGG_FUNC_t>::iterator k=avgDistFuncMap.begin();k!=avgDistFuncMap.end();k++)
		{
			if (k->second->fAuxColumnIndex == (uint32_t) -1)
			{
				k->second->fAuxColumnIndex = lastCol++;
				oidsAggDist.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
				keysAggDist.push_back(k->first);
				scaleAggDist.push_back(0);
				precisionAggDist.push_back(19);
				typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
				widthAggDist.push_back(bigIntWidth);
			}
		}

		// add auxiliary fields for statistics functions
		for (uint64_t i = 0; i < functionVec2.size(); i++)
		{
			if (functionVec2[i]->fAggFunction != ROWAGG_STATS)
				continue;

			functionVec2[i]->fAuxColumnIndex = lastCol;
			uint64_t j = functionVec2[i]->fInputColumnIndex;

			// sum(x)
			oidsAggDist.push_back(oidsAggDist[j]);
			keysAggDist.push_back(keysAggDist[j]);
			scaleAggDist.push_back(0);
			precisionAggDist.push_back(0);
			typeAggDist.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggDist.push_back(sizeof(long double));
			++lastCol;

			// sum(x**2)
			oidsAggDist.push_back(oidsAggDist[j]);
			keysAggDist.push_back(keysAggDist[j]);
			scaleAggDist.push_back(0);
			precisionAggDist.push_back(0);
			typeAggDist.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggDist.push_back(sizeof(long double));
			++lastCol;
		}
	}

	// calculate the offset and create the rowaggregation, rowgroup
	posAgg.push_back(2);
	for (uint64_t i = 0; i < oidsAgg.size(); i++)
		posAgg.push_back(posAgg[i] + widthAgg[i]);
	RowGroup aggRG(oidsAgg.size(), posAgg, oidsAgg, keysAgg, typeAgg, scaleAgg, precisionAgg,
		jobInfo.stringTableThreshold);
	SP_ROWAGG_UM_t rowAgg(new RowAggregationUM(groupBy, functionVec1, &jobInfo.rm, jobInfo.umMemLimit));

	posAggDist.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggDist.size(); i++)
		posAggDist.push_back(posAggDist[i] + widthAggDist[i]);
	RowGroup aggRgDist(oidsAggDist.size(), posAggDist, oidsAggDist, keysAggDist, typeAggDist,
		scaleAggDist, precisionAggDist, jobInfo.stringTableThreshold);
	SP_ROWAGG_DIST rowAggDist(new RowAggregationDistinct(groupByNoDist, functionVec2, &jobInfo.rm, jobInfo.umMemLimit));

	// mapping the group_concat columns, if any.
	if (jobInfo.groupConcatInfo.groupConcat().size() > 0)
	{
		jobInfo.groupConcatInfo.mapColumns(projRG);
		rowAgg->groupConcat(jobInfo.groupConcatInfo.groupConcat());
		rowAggDist->groupConcat(jobInfo.groupConcatInfo.groupConcat());
	}

	// if distinct key word applied to more than one aggregate column, reset rowAggDist
	vector<RowGroup> subRgVec;
	if (jobInfo.distinctColVec.size() > 1)
	{
		RowAggregationMultiDistinct* multiDistinctAggregator =
			new RowAggregationMultiDistinct(groupByNoDist, functionVec2, &jobInfo.rm, jobInfo.umMemLimit);
		rowAggDist.reset(multiDistinctAggregator);
		rowAggDist->groupConcat(jobInfo.groupConcatInfo.groupConcat());

		// construct and add sub-aggregators to rowAggDist
		vector<uint32_t> posAggGb, posAggSub;
		vector<uint32_t> oidsAggGb, oidsAggSub;
		vector<uint32_t> keysAggGb, keysAggSub;
		vector<uint32_t> scaleAggGb, scaleAggSub;
		vector<uint32_t> precisionAggGb, precisionAggSub;
		vector<CalpontSystemCatalog::ColDataType> typeAggGb, typeAggSub;
		vector<uint32_t> widthAggGb, widthAggSub;

		// populate groupby column info
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			oidsAggGb.push_back(oidsProj[i]);
			keysAggGb.push_back(keysProj[i]);
			scaleAggGb.push_back(scaleProj[i]);
			precisionAggGb.push_back(precisionProj[i]);
			typeAggGb.push_back(typeProj[i]);
			widthAggGb.push_back(widthProj[i]);
		}

		// for distinct, each column requires seperate rowgroup
		vector<SP_ROWAGG_DIST> rowAggSubDistVec;
		for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
		{
			uint32_t distinctColKey = jobInfo.distinctColVec[i];
			uint64_t j = -1;

			// locate the distinct key in the row group
			for (uint64_t k = 0; k < keysAgg.size(); k++)
			{
				if (keysProj[k] == distinctColKey)
				{
					j = k;
					break;
				}
			}
			idbassert(j != (uint64_t) -1);

			oidsAggSub = oidsAggGb;
			keysAggSub = keysAggGb;
			scaleAggSub = scaleAggGb;
			precisionAggSub = precisionAggGb;
			typeAggSub = typeAggGb;
			widthAggSub = widthAggGb;

			oidsAggSub.push_back(oidsProj[j]);
			keysAggSub.push_back(keysProj[j]);
			scaleAggSub.push_back(scaleProj[j]);
			precisionAggSub.push_back(precisionProj[j]);
			typeAggSub.push_back(typeProj[j]);
			widthAggSub.push_back(widthProj[j]);

			// construct sub-rowgroup
			posAggSub.clear();
			posAggSub.push_back(2);   // rid
			for (uint64_t k = 0; k < oidsAggSub.size(); k++)
				posAggSub.push_back(posAggSub[k] + widthAggSub[k]);
			RowGroup subRg(oidsAggSub.size(), posAggSub, oidsAggSub, keysAggSub, typeAggSub,
				scaleAggSub, precisionAggSub, jobInfo.stringTableThreshold);
			subRgVec.push_back(subRg);

			// construct groupby vector
			vector<SP_ROWAGG_GRPBY_t> groupBySub;
			uint64_t k = 0;
			while (k < jobInfo.groupByColVec.size())
			{
				SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(k, k));
				groupBySub.push_back(groupby);
				k++;
			}

			// add the distinct column as groupby
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(j, k));
			groupBySub.push_back(groupby);

			// tricky part : 2 function vectors
			//   -- dummy function vector for sub-aggregator, which does distinct only
			//   -- aggregate function on this distinct column for rowAggDist
			vector<SP_ROWAGG_FUNC_t> functionSub1, functionSub2;
			for (uint64_t k = 0; k < returnedColVec.size(); k++)
			{
				if (returnedColVec[k].first != distinctColKey)
					continue;

				// search the function in functionVec
				vector<SP_ROWAGG_FUNC_t>::iterator it = functionVec2.begin();
				while (it != functionVec2.end())
				{
					SP_ROWAGG_FUNC_t f = *it++;
					if ((f->fOutputColumnIndex == k) &&
						(f->fAggFunction == ROWAGG_COUNT_DISTINCT_COL_NAME ||
						 f->fAggFunction == ROWAGG_DISTINCT_SUM ||
						 f->fAggFunction == ROWAGG_DISTINCT_AVG))
					{
						SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
							f->fAggFunction,
							f->fStatsFunction,
							groupBySub.size()-1,
							f->fOutputColumnIndex,
							f->fAuxColumnIndex));
						functionSub2.push_back(funct);
					}
				}
			}

			// construct sub-aggregator
			SP_ROWAGG_UM_t subAgg(
							new RowAggregationSubDistinct(groupBySub, functionSub1, &jobInfo.rm, jobInfo.umMemLimit));
			subAgg->groupConcat(jobInfo.groupConcatInfo.groupConcat());

			// add to rowAggDist
			multiDistinctAggregator->addSubAggregator(subAgg, subRg, functionSub2);
		}

		// cover any non-distinct column functions
		{
			vector<SP_ROWAGG_FUNC_t> functionSub1 = functionNoDistVec;
			vector<SP_ROWAGG_FUNC_t> functionSub2;
			for (uint64_t k = 0; k < returnedColVec.size(); k++)
			{
				// search non-distinct functions in functionVec
				vector<SP_ROWAGG_FUNC_t>::iterator it = functionVec2.begin();
				while (it != functionVec2.end())
				{
					SP_ROWAGG_FUNC_t f = *it++;
					if ((f->fOutputColumnIndex == k) &&
						(f->fAggFunction == ROWAGG_COUNT_ASTERISK ||
						 f->fAggFunction == ROWAGG_COUNT_COL_NAME ||
						 f->fAggFunction == ROWAGG_SUM ||
						 f->fAggFunction == ROWAGG_AVG ||
						 f->fAggFunction == ROWAGG_MIN ||
						 f->fAggFunction == ROWAGG_MAX ||
						 f->fAggFunction == ROWAGG_STATS   ||
						 f->fAggFunction == ROWAGG_BIT_AND ||
						 f->fAggFunction == ROWAGG_BIT_OR  ||
						 f->fAggFunction == ROWAGG_BIT_XOR ||
						 f->fAggFunction == ROWAGG_CONSTANT ||
						 f->fAggFunction == ROWAGG_GROUP_CONCAT))
					{
						SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
							f->fAggFunction,
							f->fStatsFunction,
							f->fInputColumnIndex,
							f->fOutputColumnIndex,
							f->fAuxColumnIndex));
						functionSub2.push_back(funct);
					}
				}
			}


			if (functionSub1.size() > 0)
			{
				// make sure the group by columns are available for next aggregate phase.
				vector<SP_ROWAGG_GRPBY_t> groupBySubNoDist;
				for (uint64_t i = 0; i < groupByNoDist.size(); i++)
					groupBySubNoDist.push_back(SP_ROWAGG_GRPBY_t(
						new RowAggGroupByCol(groupByNoDist[i]->fInputColumnIndex, i)));

				// construct sub-aggregator
				SP_ROWAGG_UM_t subAgg(
					new RowAggregationUM(groupBySubNoDist, functionSub1, &jobInfo.rm, jobInfo.umMemLimit));
				subAgg->groupConcat(jobInfo.groupConcatInfo.groupConcat());

				// add to rowAggDist
				multiDistinctAggregator->addSubAggregator(subAgg, aggRG, functionSub2);
				subRgVec.push_back(aggRG);
			}
		}
	}

	rowAggDist->addAggregator(rowAgg, aggRG);
	rowgroups.push_back(aggRgDist);
	aggregators.push_back(rowAggDist);

	if (jobInfo.trace)
	{
		cout << "projected  RG: " << projRG.toString() << endl
			 << "aggregated RG: " << aggRG.toString() << endl;

		for (uint64_t i = 0; i < subRgVec.size(); i++)
		 	cout << "aggregatedSub RG: " << i << " " << subRgVec[i].toString() << endl;

	 	cout << "aggregatedDist RG: " << aggRgDist.toString() << endl;
	}
}


void TupleAggregateStep::prep2PhasesAggregate(
	JobInfo& jobInfo, vector<RowGroup>& rowgroups, vector<SP_ROWAGG_t>& aggregators)
{
	// check if there are any aggregate columns
	// a vector that has the aggregate function to be done by PM
	vector<pair<uint32_t, int> > aggColVec;
	set<uint32_t> avgSet;
	vector<std::pair<uint32_t, int> >& returnedColVec = jobInfo.returnedColVec;
	for (uint64_t i = 0; i < returnedColVec.size(); i++)
	{
		// skip if not an aggregation column
		if (returnedColVec[i].second == 0)
			continue;

		aggColVec.push_back(returnedColVec[i]);

		// remember if a column has an average function,
		// with avg function, no need for separate sum or count_column_name
		if (returnedColVec[i].second == AggregateColumn::AVG)
			avgSet.insert(returnedColVec[i].first);
	}

	// populate the aggregate rowgroup on PM and UM
	// PM: projectedRG   -> aggregateRGPM
	// UM: aggregateRGPM -> aggregateRGUM
	//
	// Aggregate preparation by joblist factory:
	// 1. get projected rowgroup (done by doAggProject) -- input to PM AGG
	// 2. construct aggregate rowgroup  -- output of PM, input of UM
	// 3. construct aggregate rowgroup  -- output of UM
	const RowGroup projRG = rowgroups[0];
	const vector<uint32_t>& oidsProj = projRG.getOIDs();
	const vector<uint32_t>& keysProj = projRG.getKeys();
	const vector<uint32_t>& scaleProj = projRG.getScale();
	const vector<uint32_t>& precisionProj = projRG.getPrecision();
	const vector<CalpontSystemCatalog::ColDataType>& typeProj = projRG.getColTypes();

	vector<uint32_t> posAggPm, posAggUm;
	vector<uint32_t> oidsAggPm, oidsAggUm;
	vector<uint32_t> keysAggPm, keysAggUm;
	vector<uint32_t> scaleAggPm, scaleAggUm;
	vector<uint32_t> precisionAggPm, precisionAggUm;
	vector<CalpontSystemCatalog::ColDataType> typeAggPm, typeAggUm;
	vector<uint32_t> widthAggPm, widthAggUm;
	vector<SP_ROWAGG_GRPBY_t> groupByPm, groupByUm;
	vector<SP_ROWAGG_FUNC_t> functionVecPm, functionVecUm;
	uint32_t bigIntWidth = sizeof(int64_t);
	uint32_t bigUintWidth = sizeof(uint64_t);
	map<pair<uint32_t, int>, uint64_t> aggFuncMap;

	// associate the columns between projected RG and aggregate RG on PM
	// populated the aggregate columns
	//     the groupby columns are put in front, even not a returned column
	//     sum and count(column name) are omitted, if avg present
	{
		// project only uniq oids, but they may be repeated in aggregation
		// collect the projected column info, prepare for aggregation
		vector<uint32_t> width;
		map<uint32_t, int> projColPosMap;
		for (uint64_t i = 0; i < keysProj.size(); i++)
		{
			projColPosMap.insert(make_pair(keysProj[i], i));
			width.push_back(projRG.getColumnWidth(i));
		}

		// column index for PM aggregate rowgroup
		uint64_t colAggPm = 0;

		// for groupby column
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			uint32_t key = jobInfo.groupByColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep2PhasesAggregate: groupby " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			uint64_t colProj = projColPosMap[key];

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAggPm));
			groupByPm.push_back(groupby);

			// PM: just copy down to aggregation rowgroup
			oidsAggPm.push_back(oidsProj[colProj]);
			keysAggPm.push_back(key);
			scaleAggPm.push_back(scaleProj[colProj]);
			precisionAggPm.push_back(precisionProj[colProj]);
			typeAggPm.push_back(typeProj[colProj]);
			widthAggPm.push_back(width[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAggPm[colAggPm], 0), colAggPm));
			colAggPm++;
		}

		// for distinct column
		for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
		{
			uint32_t key = jobInfo.distinctColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep2PhasesAggregate: distinct " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			uint64_t colProj = projColPosMap[key];

			// check for dup distinct column -- @bug6126
			if (find(keysAggPm.begin(), keysAggPm.end(), key) != keysAggPm.end())
				continue;

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAggPm));
			groupByPm.push_back(groupby);

			// PM: just copy down to aggregation rowgroup
			oidsAggPm.push_back(oidsProj[colProj]);
			keysAggPm.push_back(key);
			scaleAggPm.push_back(scaleProj[colProj]);
			precisionAggPm.push_back(precisionProj[colProj]);
			typeAggPm.push_back(typeProj[colProj]);
			widthAggPm.push_back(width[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAggPm[colAggPm], 0), colAggPm));
			colAggPm++;
		}

		// vectors for aggregate functions
		for (uint64_t i = 0; i < aggColVec.size(); i++)
		{
			uint32_t aggKey = aggColVec[i].first;
			RowAggFunctionType aggOp = functionIdMap(aggColVec[i].second);
			RowAggFunctionType stats = statsFuncIdMap(aggColVec[i].second);

			// skip on PM if this is a constant
			if (aggOp == ROWAGG_CONSTANT)
				continue;

			if (projColPosMap.find(aggKey) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[aggKey] << "' isn't in tuple.";
				cerr << "prep2PhasesAggregate: aggregate " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[aggKey].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[aggKey].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			if ((aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME) &&
				(avgSet.find(aggKey) != avgSet.end()))
				// skip sum / count(column) if avg is also selected
				continue;

			// skip if this is a duplicate
			if (aggFuncMap.find(make_pair(aggKey, aggOp)) != aggFuncMap.end())
				continue;

			uint64_t colProj = projColPosMap[aggKey];
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colProj, colAggPm));
			functionVecPm.push_back(funct);

			aggFuncMap.insert(make_pair(make_pair(aggKey, aggOp), colAggPm));

			switch (aggOp)
			{
				case ROWAGG_MIN:
				case ROWAGG_MAX:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(scaleProj[colProj]);
					precisionAggPm.push_back(precisionProj[colProj]);
					typeAggPm.push_back(typeProj[colProj]);
					widthAggPm.push_back(width[colProj]);
					colAggPm++;
				}
				break;

				case ROWAGG_SUM:
				case ROWAGG_AVG:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("sum/average");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep2PhasesAggregate: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					if (typeProj[colProj] == CalpontSystemCatalog::DOUBLE ||
						typeProj[colProj] == CalpontSystemCatalog::UDOUBLE ||
						typeProj[colProj] == CalpontSystemCatalog::FLOAT ||
						typeProj[colProj] == CalpontSystemCatalog::UFLOAT)
					{
						typeAggPm.push_back(typeProj[colProj]);
						scaleAggPm.push_back(scaleProj[colProj]);
						precisionAggPm.push_back(precisionProj[colProj]);
						widthAggPm.push_back(width[colProj]);
					}
					else if (isUnsigned(typeProj[colProj]))
					{
						typeAggPm.push_back(CalpontSystemCatalog::UBIGINT);
						uint32_t scale = scaleProj[colProj];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_AVG)
							scale = jobInfo.scaleOfAvg[aggKey]; // scale += 4;
						scaleAggPm.push_back(scale);
						precisionAggPm.push_back(20);
						widthAggPm.push_back(bigUintWidth);
					}
					else
					{
						typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
						uint32_t scale = scaleProj[colProj];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_AVG)
							scale = jobInfo.scaleOfAvg[aggKey]; // scale += 4;
						scaleAggPm.push_back(scale);
						precisionAggPm.push_back(19);
						widthAggPm.push_back(bigIntWidth);
					}
					colAggPm++;
				}
				// PM: put the count column for avg next to the sum
				// let fall through to add a count column for average function
				if (aggOp != ROWAGG_AVG)
					break;

				case ROWAGG_COUNT_ASTERISK:
				case ROWAGG_COUNT_COL_NAME:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					// work around count() in select subquery
					precisionAggPm.push_back(9999);
					typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
					widthAggPm.push_back(bigIntWidth);
					colAggPm++;
				}
				break;

				case ROWAGG_STATS:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("variance/standard deviation");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep2PhaseAggregate:: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					// counts(x)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(scaleProj[colProj]);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::DOUBLE);
					widthAggPm.push_back(sizeof(double));
					funct->fAuxColumnIndex = ++colAggPm;

					// sum(x)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAggPm.push_back(sizeof(long double));
					++colAggPm;

					// sum(x**2)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAggPm.push_back(sizeof(long double));
					++colAggPm;
				}
				break;

				case ROWAGG_BIT_AND:
				case ROWAGG_BIT_OR:
				case ROWAGG_BIT_XOR:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(-16);  // for connector to skip null check
					typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
					widthAggPm.push_back(bigIntWidth);
					colAggPm++;
				}
				break;

				default:
				{
					ostringstream emsg;
					emsg << "aggregate function (" << (uint64_t) aggOp << ") isn't supported";
					cerr << "prep2PhasesAggregate: " << emsg.str() << endl;
					throw QueryDataExcept(emsg.str(), aggregateFuncErr);
				}
			}
		}
	}

	// associate the columns between the aggregate RGs on PM and UM
	// populated the returned columns
	//     remove not returned groupby column
	//     add back sum or count(column name) if omitted due to avg column
	//     put count(column name) column to the end, if it is for avg only
	{
		// check if the count column for AVG is also a returned column,
		// if so, replace the "-1" to actual position in returned vec.
		map<uint32_t, SP_ROWAGG_FUNC_t> avgFuncMap;
		map<pair<uint32_t, RowAggFunctionType>, uint64_t> aggDupFuncMap;

		// copy over the groupby vector
		// update the outputColumnIndex if returned
		for (uint64_t i = 0; i < groupByPm.size(); i++)
		{
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(groupByPm[i]->fOutputColumnIndex, -1));
			groupByUm.push_back(groupby);
		}

		// locate the return column position in aggregated rowgroup from PM
		for (uint64_t i = 0; i < returnedColVec.size(); i++)
		{
			uint32_t retKey = returnedColVec[i].first;
			RowAggFunctionType aggOp = functionIdMap(returnedColVec[i].second);
			RowAggFunctionType stats = statsFuncIdMap(returnedColVec[i].second);
			int colPm = -1;

			map<pair<uint32_t, int>, uint64_t>::iterator it =
				aggFuncMap.find(make_pair(retKey, aggOp));
			if (it != aggFuncMap.end())
			{
				colPm = it->second;
				oidsAggUm.push_back(oidsAggPm[colPm]);
				keysAggUm.push_back(retKey);
				scaleAggUm.push_back(scaleAggPm[colPm]);
				precisionAggUm.push_back(precisionAggPm[colPm]);
				typeAggUm.push_back(typeAggPm[colPm]);
				widthAggUm.push_back(widthAggPm[colPm]);
			}

			// not a direct hit -- a returned column is not already in the RG from PMs
			else
			{
				bool returnColMissing = true;

				// check if a SUM or COUNT covered by AVG
				if (aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME)
				{
					it = aggFuncMap.find(make_pair(returnedColVec[i].first, ROWAGG_AVG));
					if (it != aggFuncMap.end())
					{
						// false alarm
						returnColMissing = false;

						colPm = it->second;
						if (aggOp == ROWAGG_SUM)
						{
							oidsAggUm.push_back(oidsAggPm[colPm]);
							keysAggUm.push_back(retKey);
							scaleAggUm.push_back(scaleAggPm[colPm] >> 8);
							precisionAggUm.push_back(precisionAggPm[colPm]);
							typeAggUm.push_back(typeAggPm[colPm]);
							widthAggUm.push_back(widthAggPm[colPm]);
						}
						else
						{
							// leave the count() to avg
							aggOp = ROWAGG_COUNT_NO_OP;

							colPm++;
							oidsAggUm.push_back(oidsAggPm[colPm]);
							keysAggUm.push_back(retKey);
							scaleAggUm.push_back(0);
							precisionAggUm.push_back(19);
							typeAggUm.push_back(CalpontSystemCatalog::BIGINT);
							widthAggUm.push_back(bigIntWidth);
						}
					}
				}
				else if (find(jobInfo.expressionVec.begin(), jobInfo.expressionVec.end(),
						 retKey) != jobInfo.expressionVec.end())
				{
					// a function on aggregation
					TupleInfo ti = getTupleInfo(retKey, jobInfo);
					oidsAggUm.push_back(ti.oid);
					keysAggUm.push_back(retKey);
					scaleAggUm.push_back(ti.scale);
					precisionAggUm.push_back(ti.precision);
					typeAggUm.push_back(ti.dtype);
					widthAggUm.push_back(ti.width);

					returnColMissing = false;
				}
				else if (jobInfo.windowSet.find(retKey) != jobInfo.windowSet.end())
				{
					// an window function
					TupleInfo ti = getTupleInfo(retKey, jobInfo);
					oidsAggUm.push_back(ti.oid);
					keysAggUm.push_back(retKey);
					scaleAggUm.push_back(ti.scale);
					precisionAggUm.push_back(ti.precision);
					typeAggUm.push_back(ti.dtype);
					widthAggUm.push_back(ti.width);

					returnColMissing = false;
				}
				else if (aggOp == ROWAGG_CONSTANT)
				{
					TupleInfo ti = getTupleInfo(retKey, jobInfo);
					oidsAggUm.push_back(ti.oid);
					keysAggUm.push_back(retKey);
					scaleAggUm.push_back(ti.scale);
					precisionAggUm.push_back(ti.precision);
					typeAggUm.push_back(ti.dtype);
					widthAggUm.push_back(ti.width);

					returnColMissing = false;
				}


				if (returnColMissing)
				{
					Message::Args args;
					args.add(keyName(i, retKey, jobInfo));
					string emsg = IDBErrorInfo::instance()->
										errorMsg(ERR_NOT_GROUPBY_EXPRESSION, args);
					cerr << "prep2PhasesAggregate: " << emsg << " oid="
						 << (int) jobInfo.keyInfo->tupleKeyVec[retKey].fId << ", alias="
						 << jobInfo.keyInfo->tupleKeyVec[retKey].fTable << ", view="
						 << jobInfo.keyInfo->tupleKeyVec[retKey].fView << ", function="
						 << (int) aggOp << endl;
					throw IDBExcept(emsg, ERR_NOT_GROUPBY_EXPRESSION);
				}
			}

			// update groupby vector if the groupby column is a returned column
			if (returnedColVec[i].second == 0)
			{
				int dupGroupbyIndex = -1;
				for (uint64_t j = 0; j < jobInfo.groupByColVec.size(); j++)
				{
					if (jobInfo.groupByColVec[j] == retKey)
					{
						if (groupByUm[j]->fOutputColumnIndex == (uint32_t) -1)
							groupByUm[j]->fOutputColumnIndex = i;
						else
							dupGroupbyIndex = groupByUm[j]->fOutputColumnIndex;
					}
				}

				for (uint64_t j = 0; j < jobInfo.distinctColVec.size(); j++)
				{
					if (jobInfo.distinctColVec[j] == retKey)
					{
						if (groupByUm[j]->fOutputColumnIndex == (uint32_t) -1)
							groupByUm[j]->fOutputColumnIndex = i;
						else
							dupGroupbyIndex = groupByUm[j]->fOutputColumnIndex;
					}
				}

				// a duplicate group by column
				if (dupGroupbyIndex != -1)
					functionVecUm.push_back(SP_ROWAGG_FUNC_t(new RowAggFunctionCol(
						ROWAGG_DUP_FUNCT, ROWAGG_FUNCT_UNDEFINE, -1, i, dupGroupbyIndex)));
			}

			// update the aggregate function vector
			else
			{
				SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colPm, i));
				if (aggOp == ROWAGG_COUNT_NO_OP)
					funct->fAuxColumnIndex = colPm;
				else if (aggOp == ROWAGG_CONSTANT)
					funct->fAuxColumnIndex = jobInfo.cntStarPos;
				functionVecUm.push_back(funct);

				// find if this func is a duplicate
				map<pair<uint32_t, RowAggFunctionType>, uint64_t>::iterator iter =
					aggDupFuncMap.find(make_pair(retKey, aggOp));
				if (iter != aggDupFuncMap.end())
				{
					if (funct->fAggFunction == ROWAGG_AVG)
						funct->fAggFunction = ROWAGG_DUP_AVG;
					else if (funct->fAggFunction == ROWAGG_STATS)
						funct->fAggFunction = ROWAGG_DUP_STATS;
					else
						funct->fAggFunction = ROWAGG_DUP_FUNCT;
					funct->fAuxColumnIndex = iter->second;
				}
				else
				{
					aggDupFuncMap.insert(make_pair(make_pair(retKey, aggOp),
																funct->fOutputColumnIndex));
				}

				if (returnedColVec[i].second == AggregateColumn::AVG)
					avgFuncMap.insert(make_pair(returnedColVec[i].first, funct));
			}
		}

		// now fix the AVG function, locate the count(column) position
		for (uint64_t i = 0; i < functionVecUm.size(); i++)
		{
			if (functionVecUm[i]->fAggFunction != ROWAGG_COUNT_NO_OP)
				continue;

			// if the count(k) can be associated with an avg(k)
			map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
				avgFuncMap.find(keysAggUm[functionVecUm[i]->fOutputColumnIndex]);
			if (k != avgFuncMap.end())
				k->second->fAuxColumnIndex = functionVecUm[i]->fOutputColumnIndex;
		}

		// there is avg(k), but no count(k) in the select list
		uint64_t lastCol = returnedColVec.size();
		for (map<uint32_t,SP_ROWAGG_FUNC_t>::iterator k=avgFuncMap.begin();k!=avgFuncMap.end();k++)
		{
			if (k->second->fAuxColumnIndex == (uint32_t) -1)
			{
				k->second->fAuxColumnIndex = lastCol++;
				oidsAggUm.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
				keysAggUm.push_back(k->first);
				scaleAggUm.push_back(0);
				precisionAggUm.push_back(19);
				typeAggUm.push_back(CalpontSystemCatalog::BIGINT);
				widthAggUm.push_back(bigIntWidth);
			}
		}

		// add auxiliary fields for statistics functions
		for (uint64_t i = 0; i < functionVecUm.size(); i++)
		{
			if (functionVecUm[i]->fAggFunction != ROWAGG_STATS)
				continue;

			functionVecUm[i]->fAuxColumnIndex = lastCol;
			uint64_t j = functionVecUm[i]->fInputColumnIndex;

			// sum(x)
			oidsAggUm.push_back(oidsAggUm[j]);
			keysAggUm.push_back(keysAggUm[j]);
			scaleAggUm.push_back(0);
			precisionAggUm.push_back(0);
			typeAggUm.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggUm.push_back(sizeof(long double));
			++lastCol;

			// sum(x**2)
			oidsAggUm.push_back(oidsAggUm[j]);
			keysAggUm.push_back(keysAggUm[j]);
			scaleAggUm.push_back(0);
			precisionAggUm.push_back(0);
			typeAggUm.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggUm.push_back(sizeof(long double));
			++lastCol;
		}
	}

	// calculate the offset and create the rowaggregations, rowgroups
	posAggUm.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggUm.size(); i++)
		posAggUm.push_back(posAggUm[i] + widthAggUm[i]);
	RowGroup aggRgUm(oidsAggUm.size(), posAggUm, oidsAggUm, keysAggUm, typeAggUm, scaleAggUm,
		precisionAggUm, jobInfo.stringTableThreshold);
	SP_ROWAGG_UM_t rowAggUm(new RowAggregationUMP2(groupByUm, functionVecUm, &jobInfo.rm, jobInfo.umMemLimit));
	rowgroups.push_back(aggRgUm);
	aggregators.push_back(rowAggUm);

	posAggPm.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggPm.size(); i++)
		posAggPm.push_back(posAggPm[i] + widthAggPm[i]);
	RowGroup aggRgPm(oidsAggPm.size(), posAggPm, oidsAggPm, keysAggPm, typeAggPm, scaleAggPm,
		precisionAggPm, jobInfo.stringTableThreshold);
	SP_ROWAGG_PM_t rowAggPm(new RowAggregation(groupByPm, functionVecPm));
	rowgroups.push_back(aggRgPm);
	aggregators.push_back(rowAggPm);

	if (jobInfo.trace)
		cout << "\n====== Aggregation RowGroups ======" << endl
			 << "projected   RG: " << projRG.toString() << endl
			 << "aggregated1 RG: " << aggRgPm.toString() << endl
			 << "aggregated2 RG: " << aggRgUm.toString() << endl;
}


void TupleAggregateStep::prep2PhasesDistinctAggregate(
							JobInfo& jobInfo,
							vector<RowGroup>& rowgroups,
							vector<SP_ROWAGG_t>& aggregators)
{
	// check if there are any aggregate columns
	// a vector that has the aggregate function to be done by PM
	vector<pair<uint32_t, int> > aggColVec, aggNoDistColVec;
	set<uint32_t> avgSet, avgDistSet;
	vector<std::pair<uint32_t, int> >& returnedColVec = jobInfo.returnedColVec;
	for (uint64_t i = 0; i < returnedColVec.size(); i++)
	{
		// col should be an aggregate or groupBy or window function
		uint32_t rtcKey = returnedColVec[i].first;
		uint32_t rtcOp = returnedColVec[i].second;
		if (rtcOp == 0 &&
			find(jobInfo.distinctColVec.begin(),jobInfo.distinctColVec.end(), rtcKey) !=
				jobInfo.distinctColVec.end() &&
			find(jobInfo.groupByColVec.begin(),jobInfo.groupByColVec.end(), rtcKey ) ==
				jobInfo.groupByColVec.end() &&
			jobInfo.windowSet.find(rtcKey) != jobInfo.windowSet.end())
		{
			Message::Args args;
			args.add(keyName(i, rtcKey, jobInfo));
			string emsg = IDBErrorInfo::instance()->errorMsg(ERR_NOT_GROUPBY_EXPRESSION, args);
			cerr << "prep2PhasesDistinctAggregate: " << emsg << " oid="
				 << (int) jobInfo.keyInfo->tupleKeyVec[rtcKey].fId << ", alias="
				 << jobInfo.keyInfo->tupleKeyVec[rtcKey].fTable << ", view="
				 << jobInfo.keyInfo->tupleKeyVec[rtcKey].fView << endl;
			throw IDBExcept(emsg, ERR_NOT_GROUPBY_EXPRESSION);
		}

		// skip if not an aggregation column
		if (returnedColVec[i].second == 0)
			continue;

		aggColVec.push_back(returnedColVec[i]);

		// remember if a column has an average function,
		// with avg function, no need for separate sum or count_column_name
		if (returnedColVec[i].second == AggregateColumn::AVG)
			avgSet.insert(returnedColVec[i].first);
		if ( returnedColVec[i].second == AggregateColumn::DISTINCT_AVG)
			avgDistSet.insert(returnedColVec[i].first);
	}

	// populate the aggregate rowgroup on PM and UM
	// PM: projectedRG   -> aggregateRGPM
	// UM: aggregateRGPM -> aggregateRGUM
	//
	// Aggregate preparation by joblist factory:
	// 1. get projected rowgroup (done by doAggProject) -- input to PM AGG
	// 2. construct aggregate rowgroup  -- output of PM, input of UM
	// 3. construct aggregate rowgroup  -- output of UM
	// 4. construct aggregate rowgroup  -- output of distinct aggregates

	const RowGroup projRG = rowgroups[0];
	const vector<uint32_t>& oidsProj = projRG.getOIDs();
	const vector<uint32_t>& keysProj = projRG.getKeys();
	const vector<uint32_t>& scaleProj = projRG.getScale();
	const vector<uint32_t>& precisionProj = projRG.getPrecision();
	const vector<CalpontSystemCatalog::ColDataType>& typeProj = projRG.getColTypes();

	vector<uint32_t> posAggPm, posAggUm, posAggDist;
	vector<uint32_t> oidsAggPm, oidsAggUm, oidsAggDist;
	vector<uint32_t> keysAggPm, keysAggUm, keysAggDist;
	vector<uint32_t> scaleAggPm, scaleAggUm, scaleAggDist;
	vector<uint32_t> precisionAggPm, precisionAggUm, precisionAggDist;
	vector<CalpontSystemCatalog::ColDataType> typeAggPm, typeAggUm, typeAggDist;
	vector<uint32_t> widthAggPm, widthAggUm, widthAggDist;

	vector<SP_ROWAGG_GRPBY_t> groupByPm, groupByUm, groupByNoDist;
	vector<SP_ROWAGG_FUNC_t> functionVecPm, functionNoDistVec, functionVecUm;

	uint32_t bigIntWidth = sizeof(int64_t);
	map<pair<uint32_t, int>, uint64_t> aggFuncMap, avgFuncDistMap;

	// associate the columns between projected RG and aggregate RG on PM
	// populated the aggregate columns
	//     the groupby columns are put in front, even not a returned column
	//     sum and count(column name) are omitted, if avg present
	{
		// project only uniq oids, but they may be repeated in aggregation
		// collect the projected column info, prepare for aggregation
		vector<uint32_t> width;
		map<uint32_t, int> projColPosMap;
		for (uint64_t i = 0; i < keysProj.size(); i++)
		{
			projColPosMap.insert(make_pair(keysProj[i], i));
			width.push_back(projRG.getColumnWidth(i));
		}

		// column index for PM aggregate rowgroup
		uint64_t colAggPm = 0;

		// for groupby column
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			uint32_t key = jobInfo.groupByColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep2PhasesDistinctAggregate: group " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			uint64_t colProj = projColPosMap[key];

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAggPm));
			groupByPm.push_back(groupby);

			// PM: just copy down to aggregation rowgroup
			oidsAggPm.push_back(oidsProj[colProj]);
			keysAggPm.push_back(key);
			scaleAggPm.push_back(scaleProj[colProj]);
			precisionAggPm.push_back(precisionProj[colProj]);
			typeAggPm.push_back(typeProj[colProj]);
			widthAggPm.push_back(width[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAggPm[colAggPm], 0), colAggPm));
			colAggPm++;
		}

		// for distinct column
		for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
		{
			uint32_t key = jobInfo.distinctColVec[i];
			if (projColPosMap.find(key) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' isn't in tuple.";
				cerr << "prep2PhasesDistinctAggregate: distinct " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[key].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[key].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[key].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[key].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			// check for dup distinct column -- @bug6126
			if (find(keysAggPm.begin(), keysAggPm.end(), key) != keysAggPm.end())
				continue;

			uint64_t colProj = projColPosMap[key];

			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(colProj, colAggPm));
			groupByPm.push_back(groupby);

			// PM: just copy down to aggregation rowgroup
			oidsAggPm.push_back(oidsProj[colProj]);
			keysAggPm.push_back(key);
			scaleAggPm.push_back(scaleProj[colProj]);
			precisionAggPm.push_back(precisionProj[colProj]);
			typeAggPm.push_back(typeProj[colProj]);
			widthAggPm.push_back(width[colProj]);

			aggFuncMap.insert(make_pair(make_pair(keysAggPm[colAggPm], 0), colAggPm));
			colAggPm++;
		}

		// vectors for aggregate functions
		for (uint64_t i = 0; i < aggColVec.size(); i++)
		{
			// skip on PM if this is a constant
			RowAggFunctionType aggOp = functionIdMap(aggColVec[i].second);
			if (aggOp == ROWAGG_CONSTANT)
				continue;

			uint32_t aggKey = aggColVec[i].first;
			if (projColPosMap.find(aggKey) == projColPosMap.end())
			{
				ostringstream emsg;
				emsg << "'" << jobInfo.keyInfo->tupleKeyToName[aggKey] << "' isn't in tuple.";
				cerr << "prep2PhasesDistinctAggregate: aggregate " << emsg.str()
					 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[aggKey].fId
					 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fTable;
				if (jobInfo.keyInfo->tupleKeyVec[aggKey].fView.length() > 0)
					 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[aggKey].fView;
				cerr << endl;
				throw logic_error(emsg.str());
			}

			RowAggFunctionType stats = statsFuncIdMap(aggColVec[i].second);
			// skip sum / count(column) if avg is also selected
			if ((aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME) &&
				(avgSet.find(aggKey) != avgSet.end()))
				continue;

			// skip if this is a duplicate
			if (aggFuncMap.find(make_pair(aggKey, aggOp)) != aggFuncMap.end())
				continue;

			if (aggOp == ROWAGG_DISTINCT_SUM ||
				aggOp == ROWAGG_DISTINCT_AVG ||
				aggOp == ROWAGG_COUNT_DISTINCT_COL_NAME)
				continue;

			uint64_t colProj = projColPosMap[aggKey];
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colProj, colAggPm));
			functionVecPm.push_back(funct);

			aggFuncMap.insert(make_pair(make_pair(aggKey, aggOp), colAggPm));

			switch (aggOp)
			{
				case ROWAGG_MIN:
				case ROWAGG_MAX:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(scaleProj[colProj]);
					precisionAggPm.push_back(precisionProj[colProj]);
					typeAggPm.push_back(typeProj[colProj]);
					widthAggPm.push_back(width[colProj]);
					colAggPm++;
				}
				break;

				case ROWAGG_SUM:
				case ROWAGG_AVG:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("sum/average");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep2PhasesDistinctAggregate: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					if (typeProj[colProj] != CalpontSystemCatalog::DOUBLE &&
						typeProj[colProj] != CalpontSystemCatalog::FLOAT)
					{
						typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
						uint32_t scale = scaleProj[colProj];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_AVG)
							scale = jobInfo.scaleOfAvg[aggKey]; // scale += 4;
						scaleAggPm.push_back(scale);
						precisionAggPm.push_back(19);
						widthAggPm.push_back(bigIntWidth);
					}
					else
					{
						typeAggPm.push_back(typeProj[colProj]);
						scaleAggPm.push_back(scaleProj[colProj]);
						precisionAggPm.push_back(precisionProj[colProj]);
						widthAggPm.push_back(width[colProj]);
					}
					colAggPm++;
				}
				// PM: put the count column for avg next to the sum
				// let fall through to add a count column for average function
				if (aggOp == ROWAGG_AVG)
					funct->fAuxColumnIndex = colAggPm;
				else
					break;

				case ROWAGG_COUNT_ASTERISK:
				case ROWAGG_COUNT_COL_NAME:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					// work around count() in select subquery
					precisionAggPm.push_back(9999);
					typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
					widthAggPm.push_back(bigIntWidth);
					colAggPm++;
				}
				break;

				case ROWAGG_STATS:
				{
					if (typeProj[colProj] == CalpontSystemCatalog::CHAR ||
						typeProj[colProj] == CalpontSystemCatalog::VARCHAR ||
						typeProj[colProj] == CalpontSystemCatalog::DATE ||
						typeProj[colProj] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("variance/standard deviation");
						args.add(colTypeIdString(typeProj[colProj]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep2PhasesDistinctAggregate:: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					// count(x)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(scaleProj[colProj]);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::DOUBLE);
					widthAggPm.push_back(sizeof(double));
					funct->fAuxColumnIndex = ++colAggPm;

					// sum(x)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAggPm.push_back(sizeof(long double));
					++colAggPm;

					// sum(x**2)
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(0);
					typeAggPm.push_back(CalpontSystemCatalog::LONGDOUBLE);
					widthAggPm.push_back(sizeof(long double));
					++colAggPm;
				}
				break;

				case ROWAGG_BIT_AND:
				case ROWAGG_BIT_OR:
				case ROWAGG_BIT_XOR:
				{
					oidsAggPm.push_back(oidsProj[colProj]);
					keysAggPm.push_back(aggKey);
					scaleAggPm.push_back(0);
					precisionAggPm.push_back(-16);  // for connector to skip null check
					typeAggPm.push_back(CalpontSystemCatalog::BIGINT);
					widthAggPm.push_back(bigIntWidth);
					colAggPm++;
				}
				break;

				default:
				{
					ostringstream emsg;
					emsg << "aggregate function (" << (uint64_t) aggOp << ") isn't supported";
					cerr << "prep2PhasesDistinctAggregate: " << emsg.str() << endl;
					throw QueryDataExcept(emsg.str(), aggregateFuncErr);
				}
			}
		}
	}


	// associate the columns between the aggregate RGs on PM and UM without distinct aggregator
	// populated the returned columns
	{
		for (uint32_t idx = 0; idx < groupByPm.size(); idx++)
		{
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(idx, idx));
			groupByUm.push_back(groupby);
		}

		for (uint32_t idx = 0; idx < functionVecPm.size(); idx++)
		{
			SP_ROWAGG_FUNC_t funcPm = functionVecPm[idx];
			SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(
				funcPm->fAggFunction,
				funcPm->fStatsFunction,
				funcPm->fOutputColumnIndex,
				funcPm->fOutputColumnIndex,
				funcPm->fAuxColumnIndex));
			functionNoDistVec.push_back(funct);
		}

		posAggUm = posAggPm;
		oidsAggUm = oidsAggPm;
		keysAggUm = keysAggPm;
		scaleAggUm = scaleAggPm;
		precisionAggUm = precisionAggPm;
		widthAggUm = widthAggPm;
		typeAggUm = typeAggPm;
	}


	// associate the columns between the aggregate RGs on UM and Distinct
	// populated the returned columns
	//     remove not returned groupby column
	//     add back sum or count(column name) if omitted due to avg column
	//     put count(column name) column to the end, if it is for avg only
	{
		// check if the count column for AVG is also a returned column,
		// if so, replace the "-1" to actual position in returned vec.
		map<uint32_t, SP_ROWAGG_FUNC_t> avgFuncMap, avgDistFuncMap;
		map<pair<uint32_t, RowAggFunctionType>, uint64_t> aggDupFuncMap;

		// copy over the groupby vector
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(i, -1));
			groupByNoDist.push_back(groupby);
		}

		// locate the return column position in aggregated rowgroup from PM
		for (uint64_t i = 0; i < returnedColVec.size(); i++)
		{
			uint32_t retKey = returnedColVec[i].first;

			RowAggFunctionType aggOp = functionIdMap(returnedColVec[i].second);
			RowAggFunctionType stats = statsFuncIdMap(returnedColVec[i].second);
			int colUm = -1;
			if  (find(jobInfo.distinctColVec.begin(), jobInfo.distinctColVec.end(), retKey) !=
					jobInfo.distinctColVec.end() )
			{
				map<pair<uint32_t, int>, uint64_t>::iterator it = aggFuncMap.find(make_pair(retKey, 0));
				if (it != aggFuncMap.end())
				{
					colUm = it->second;
				}
				else
				{
					ostringstream emsg;
					emsg << "'" << jobInfo.keyInfo->tupleKeyToName[retKey] << "' isn't in tuple.";
					cerr << "prep2PhasesDistinctAggregate: distinct " << emsg.str()
						 << " oid=" << (int) jobInfo.keyInfo->tupleKeyVec[retKey].fId
						 << ", alias=" << jobInfo.keyInfo->tupleKeyVec[retKey].fTable;
					if (jobInfo.keyInfo->tupleKeyVec[retKey].fView.length() > 0)
						 cerr << ", view=" << jobInfo.keyInfo->tupleKeyVec[retKey].fView;
					cerr << endl;
					throw QueryDataExcept(emsg.str(), aggregateFuncErr);
				}
			}

			switch (aggOp)
			{
				case ROWAGG_DISTINCT_AVG:
					//avgFuncMap.insert(make_pair(key, funct));
				case ROWAGG_DISTINCT_SUM:
				{
					if (typeAggUm[colUm] == CalpontSystemCatalog::CHAR ||
						typeAggUm[colUm] == CalpontSystemCatalog::VARCHAR ||
						typeAggUm[colUm] == CalpontSystemCatalog::DATE ||
						typeAggUm[colUm] == CalpontSystemCatalog::DATETIME)
					{
						Message::Args args;
						args.add("sum/average");
						args.add(colTypeIdString(typeAggUm[colUm]));
						string emsg = IDBErrorInfo::instance()->
							errorMsg(ERR_AGGREGATE_TYPE_NOT_SUPPORT, args);
						cerr << "prep2PhasesDistinctAggregate: " << emsg << endl;
						throw IDBExcept(emsg, ERR_AGGREGATE_TYPE_NOT_SUPPORT);
					}

					oidsAggDist.push_back(oidsAggUm[colUm]);
					keysAggDist.push_back(retKey);
					if (typeAggUm[colUm] != CalpontSystemCatalog::DOUBLE &&
						typeAggUm[colUm] != CalpontSystemCatalog::FLOAT)
					{
						typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
						uint32_t scale = scaleAggUm[colUm];
						// for int average, FE expects a decimal
						if (aggOp == ROWAGG_DISTINCT_AVG)
							scale = jobInfo.scaleOfAvg[retKey]; // scale += 4;
						scaleAggDist.push_back(scale);
						precisionAggDist.push_back(19);
						widthAggDist.push_back(bigIntWidth);
					}
					else
					{
						typeAggDist.push_back(typeAggUm[colUm]);
						scaleAggDist.push_back(scaleAggUm[colUm]);
						precisionAggDist.push_back(precisionAggUm[colUm]);
						widthAggDist.push_back(widthAggUm[colUm]);
					}
				}
				// PM: put the count column for avg next to the sum
				// let fall through to add a count column for average function
				//if (aggOp != ROWAGG_DISTINCT_AVG)
				break;

				case ROWAGG_COUNT_DISTINCT_COL_NAME:
				{
					oidsAggDist.push_back(oidsAggUm[colUm]);
					keysAggDist.push_back(retKey);
					scaleAggDist.push_back(0);
					// work around count() in select subquery
					precisionAggDist.push_back(9999);
					typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
					widthAggDist.push_back(bigIntWidth);
				}
				break;

				case ROWAGG_MIN:
				case ROWAGG_MAX:
				case ROWAGG_SUM:
				case ROWAGG_AVG:
				case ROWAGG_COUNT_ASTERISK:
				case ROWAGG_COUNT_COL_NAME:
				case ROWAGG_STATS:
				case ROWAGG_BIT_AND:
				case ROWAGG_BIT_OR:
				case ROWAGG_BIT_XOR:
				case ROWAGG_CONSTANT:
				default:
				{
					map<pair<uint32_t, int>, uint64_t>::iterator it =
						aggFuncMap.find(make_pair(retKey, aggOp));
					if (it != aggFuncMap.end())
					{
						colUm = it->second;
						oidsAggDist.push_back(oidsAggUm[colUm]);
						keysAggDist.push_back(keysAggUm[colUm]);
						scaleAggDist.push_back(scaleAggUm[colUm]);
						precisionAggDist.push_back(precisionAggUm[colUm]);
						typeAggDist.push_back(typeAggUm[colUm]);
						widthAggDist.push_back(widthAggUm[colUm]);
					}

					// not a direct hit -- a returned column is not already in the RG from PMs
					else
					{
						bool returnColMissing = true;

						// check if a SUM or COUNT covered by AVG
						if (aggOp == ROWAGG_SUM || aggOp == ROWAGG_COUNT_COL_NAME)
						{
							it = aggFuncMap.find(make_pair(returnedColVec[i].first, ROWAGG_AVG));
							if (it != aggFuncMap.end())
							{
								// false alarm
								returnColMissing = false;

								colUm = it->second;
								if (aggOp == ROWAGG_SUM)
								{
									oidsAggDist.push_back(oidsAggUm[colUm]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(scaleAggUm[colUm] >> 8);
									precisionAggDist.push_back(precisionAggUm[colUm]);
									typeAggDist.push_back(typeAggUm[colUm]);
									widthAggDist.push_back(widthAggUm[colUm]);
								}
								else
								{
									// leave the count() to avg
									aggOp = ROWAGG_COUNT_NO_OP;

									oidsAggDist.push_back(oidsAggUm[colUm]);
									keysAggDist.push_back(retKey);
									scaleAggDist.push_back(0);
									precisionAggDist.push_back(19);
									typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
									widthAggDist.push_back(bigIntWidth);
								}
							}
						}
						else if (find(jobInfo.expressionVec.begin(), jobInfo.expressionVec.end(),
								 retKey) != jobInfo.expressionVec.end())
						{
							// a function on aggregation
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}
						else if (jobInfo.windowSet.find(retKey) != jobInfo.windowSet.end())
						{
							// a window function
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}
						else if (aggOp == ROWAGG_CONSTANT)
						{
							TupleInfo ti = getTupleInfo(retKey, jobInfo);
							oidsAggDist.push_back(ti.oid);
							keysAggDist.push_back(retKey);
							scaleAggDist.push_back(ti.scale);
							precisionAggDist.push_back(ti.precision);
							typeAggDist.push_back(ti.dtype);
							widthAggDist.push_back(ti.width);

							returnColMissing = false;
						}

						if (returnColMissing)
						{
							Message::Args args;
							args.add(keyName(i, retKey, jobInfo));
							string emsg = IDBErrorInfo::instance()->
												errorMsg(ERR_NOT_GROUPBY_EXPRESSION, args);
							cerr << "prep2PhasesDistinctAggregate: " << emsg << " oid="
								 << (int) jobInfo.keyInfo->tupleKeyVec[retKey].fId << ", alias="
								 << jobInfo.keyInfo->tupleKeyVec[retKey].fTable << ", view="
								 << jobInfo.keyInfo->tupleKeyVec[retKey].fView << ", function="
								 << (int) aggOp << endl;
							throw IDBExcept(emsg, ERR_NOT_GROUPBY_EXPRESSION);
						}
					} //else
				} // switch
			}

			// update groupby vector if the groupby column is a returned column
			if (returnedColVec[i].second == 0)
			{
				int dupGroupbyIndex = -1;
				for (uint64_t j = 0; j < jobInfo.groupByColVec.size(); j++)
				{
					if (jobInfo.groupByColVec[j] == retKey)
					{
						if (groupByNoDist[j]->fOutputColumnIndex == (uint32_t) -1)
							groupByNoDist[j]->fOutputColumnIndex = i;
						else
							dupGroupbyIndex = groupByNoDist[j]->fOutputColumnIndex;
					}
				}

				// a duplicate group by column
				if (dupGroupbyIndex != -1)
					functionVecUm.push_back(SP_ROWAGG_FUNC_t(new RowAggFunctionCol(
						ROWAGG_DUP_FUNCT, ROWAGG_FUNCT_UNDEFINE, -1, i, dupGroupbyIndex)));
			}

			// update the aggregate function vector
			else
			{
				SP_ROWAGG_FUNC_t funct(new RowAggFunctionCol(aggOp, stats, colUm, i));
				if (aggOp == ROWAGG_COUNT_NO_OP)
					funct->fAuxColumnIndex = colUm;
				else if (aggOp == ROWAGG_CONSTANT)
					funct->fAuxColumnIndex = jobInfo.cntStarPos;

				functionVecUm.push_back(funct);

				// find if this func is a duplicate
				map<pair<uint32_t, RowAggFunctionType>, uint64_t>::iterator iter =
					aggDupFuncMap.find(make_pair(retKey, aggOp));
				if (iter != aggDupFuncMap.end())
				{
					if (funct->fAggFunction == ROWAGG_AVG)
						funct->fAggFunction = ROWAGG_DUP_AVG;
					else if (funct->fAggFunction == ROWAGG_STATS)
						funct->fAggFunction = ROWAGG_DUP_STATS;
					else
						funct->fAggFunction = ROWAGG_DUP_FUNCT;

					funct->fAuxColumnIndex = iter->second;
				}
				else
				{
					aggDupFuncMap.insert(make_pair(make_pair(retKey, aggOp),
																funct->fOutputColumnIndex));
				}

				if (returnedColVec[i].second == AggregateColumn::AVG)
					avgFuncMap.insert(make_pair(returnedColVec[i].first, funct));
				else if (returnedColVec[i].second == AggregateColumn::DISTINCT_AVG)
					avgDistFuncMap.insert(make_pair(returnedColVec[i].first, funct));
			}
		} // for (i

		// now fix the AVG function, locate the count(column) position
		for (uint64_t i = 0; i < functionVecUm.size(); i++)
		{
			// if the count(k) can be associated with an avg(k)
			if (functionVecUm[i]->fAggFunction == ROWAGG_COUNT_NO_OP) {
				map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
					avgFuncMap.find(keysAggDist[functionVecUm[i]->fOutputColumnIndex]);
				if (k != avgFuncMap.end())
					k->second->fAuxColumnIndex = functionVecUm[i]->fOutputColumnIndex;
			}
		}

		// there is avg(k), but no count(k) in the select list
		uint64_t lastCol = returnedColVec.size();
		for (map<uint32_t,SP_ROWAGG_FUNC_t>::iterator k=avgFuncMap.begin();k!=avgFuncMap.end();k++)
		{
			if (k->second->fAuxColumnIndex == (uint32_t) -1)
				{
					k->second->fAuxColumnIndex = lastCol++;
					oidsAggDist.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
					keysAggDist.push_back(k->first);
					scaleAggDist.push_back(0);
					precisionAggDist.push_back(19);
					typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
					widthAggDist.push_back(bigIntWidth);
				}
		}

		//distinct avg
		for (uint64_t i = 0; i < functionVecUm.size(); i++)
		{
			if (functionVecUm[i]->fAggFunction == ROWAGG_COUNT_DISTINCT_COL_NAME) {
				map<uint32_t, SP_ROWAGG_FUNC_t>::iterator k =
					avgDistFuncMap.find(keysAggDist[functionVecUm[i]->fOutputColumnIndex]);
				if (k != avgDistFuncMap.end())
				{
					k->second->fAuxColumnIndex = functionVecUm[i]->fOutputColumnIndex;
					functionVecUm[i]->fAggFunction = ROWAGG_COUNT_NO_OP;
				}
			}
		}

		// there is avg(distinct k), but no count(distinct k) in the select list
		for (map<uint32_t,SP_ROWAGG_FUNC_t>::iterator k=avgDistFuncMap.begin(); k!=avgDistFuncMap.end(); k++)
		{
			// find count(distinct k) or add it
			if (k->second->fAuxColumnIndex == (uint32_t) -1)
			{
				k->second->fAuxColumnIndex = lastCol++;
				oidsAggDist.push_back(jobInfo.keyInfo->tupleKeyVec[k->first].fId);
				keysAggDist.push_back(k->first);
				scaleAggDist.push_back(0);
				precisionAggDist.push_back(19);
				typeAggDist.push_back(CalpontSystemCatalog::BIGINT);
				widthAggDist.push_back(bigIntWidth);
			}
		}

		// add auxiliary fields for statistics functions
		for (uint64_t i = 0; i < functionVecUm.size(); i++)
		{
			if (functionVecUm[i]->fAggFunction != ROWAGG_STATS)
				continue;

			functionVecUm[i]->fAuxColumnIndex = lastCol;
			uint64_t j = functionVecUm[i]->fInputColumnIndex;

			// sum(x)
			oidsAggDist.push_back(oidsAggDist[j]);
			keysAggDist.push_back(keysAggDist[j]);
			scaleAggDist.push_back(0);
			precisionAggDist.push_back(0);
			typeAggDist.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggDist.push_back(sizeof(long double));
			++lastCol;

			// sum(x**2)
			oidsAggDist.push_back(oidsAggDist[j]);
			keysAggDist.push_back(keysAggDist[j]);
			scaleAggDist.push_back(0);
			precisionAggDist.push_back(0);
			typeAggDist.push_back(CalpontSystemCatalog::LONGDOUBLE);
			widthAggDist.push_back(sizeof(long double));
			++lastCol;
		}
	}


	// calculate the offset and create the rowaggregations, rowgroups
	posAggUm.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggUm.size(); i++)
		posAggUm.push_back(posAggUm[i] + widthAggUm[i]);
	RowGroup aggRgUm(oidsAggUm.size(), posAggUm, oidsAggUm, keysAggUm, typeAggUm, scaleAggUm,
		precisionAggUm, jobInfo.stringTableThreshold);
	SP_ROWAGG_UM_t rowAggUm(new RowAggregationUMP2(groupByUm, functionNoDistVec, &jobInfo.rm, jobInfo.umMemLimit));

	posAggDist.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggDist.size(); i++)
		posAggDist.push_back(posAggDist[i] + widthAggDist[i]);
	RowGroup aggRgDist(oidsAggDist.size(), posAggDist, oidsAggDist, keysAggDist, typeAggDist,
		scaleAggDist, precisionAggDist, jobInfo.stringTableThreshold);
	SP_ROWAGG_DIST rowAggDist(new RowAggregationDistinct(groupByNoDist, functionVecUm, &jobInfo.rm, jobInfo.umMemLimit));

	// if distinct key word applied to more than one aggregate column, reset rowAggDist
	vector<RowGroup> subRgVec;
	if (jobInfo.distinctColVec.size() > 1)
	{
		RowAggregationMultiDistinct* multiDistinctAggregator =
			new RowAggregationMultiDistinct(groupByNoDist, functionVecUm, &jobInfo.rm, jobInfo.umMemLimit);
		rowAggDist.reset(multiDistinctAggregator);

		// construct and add sub-aggregators to rowAggDist
		vector<uint32_t> posAggGb, posAggSub;
		vector<uint32_t> oidsAggGb, oidsAggSub;
		vector<uint32_t> keysAggGb, keysAggSub;
		vector<uint32_t> scaleAggGb, scaleAggSub;
		vector<uint32_t> precisionAggGb, precisionAggSub;
		vector<CalpontSystemCatalog::ColDataType> typeAggGb, typeAggSub;
		vector<uint32_t> widthAggGb, widthAggSub;

		// populate groupby column info
		for (uint64_t i = 0; i < jobInfo.groupByColVec.size(); i++)
		{
			oidsAggGb.push_back(oidsAggUm[i]);
			keysAggGb.push_back(keysAggUm[i]);
			scaleAggGb.push_back(scaleAggUm[i]);
			precisionAggGb.push_back(precisionAggUm[i]);
			typeAggGb.push_back(typeAggUm[i]);
			widthAggGb.push_back(widthAggUm[i]);
		}

		// for distinct, each column requires seperate rowgroup
		vector<SP_ROWAGG_DIST> rowAggSubDistVec;
		for (uint64_t i = 0; i < jobInfo.distinctColVec.size(); i++)
		{
			uint32_t distinctColKey = jobInfo.distinctColVec[i];
			uint64_t j = -1;

			// locate the distinct key in the row group
			for (uint64_t k = 0; k < keysAggUm.size(); k++)
			{
				if (keysAggUm[k] == distinctColKey)
				{
					j = k;
					break;
				}
			}
			idbassert(j != (uint64_t) -1);

			oidsAggSub = oidsAggGb;
			keysAggSub = keysAggGb;
			scaleAggSub = scaleAggGb;
			precisionAggSub = precisionAggGb;
			typeAggSub = typeAggGb;
			widthAggSub = widthAggGb;

			oidsAggSub.push_back(oidsAggUm[j]);
			keysAggSub.push_back(keysAggUm[j]);
			scaleAggSub.push_back(scaleAggUm[j]);
			precisionAggSub.push_back(precisionAggUm[j]);
			typeAggSub.push_back(typeAggUm[j]);
			widthAggSub.push_back(widthAggUm[j]);

			// construct sub-rowgroup
			posAggSub.clear();
			posAggSub.push_back(2);   // rid
			for (uint64_t k = 0; k < oidsAggSub.size(); k++)
				posAggSub.push_back(posAggSub[k] + widthAggSub[k]);
			RowGroup subRg(oidsAggSub.size(), posAggSub, oidsAggSub, keysAggSub, typeAggSub,
				scaleAggSub, precisionAggSub, jobInfo.stringTableThreshold);
			subRgVec.push_back(subRg);

			// construct groupby vector
			vector<SP_ROWAGG_GRPBY_t> groupBySub;
			uint64_t k = 0;
			while (k < jobInfo.groupByColVec.size())
			{
				SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(k, k));
				groupBySub.push_back(groupby);
				k++;
			}

			// add the distinct column as groupby
			SP_ROWAGG_GRPBY_t groupby(new RowAggGroupByCol(j, k));
			groupBySub.push_back(groupby);

			// tricky part : 2 function vectors
			//   -- dummy function vector for sub-aggregator, which does distinct only
			//   -- aggregate function on this distinct column for rowAggDist
			vector<SP_ROWAGG_FUNC_t> functionSub1, functionSub2;
			for (uint64_t k = 0; k < returnedColVec.size(); k++)
			{
				if (returnedColVec[k].first != distinctColKey)
					continue;

				// search the function in functionVec
				vector<SP_ROWAGG_FUNC_t>::iterator it = functionVecUm.begin();
				while (it != functionVecUm.end())
				{
					SP_ROWAGG_FUNC_t f = *it++;
					if ((f->fOutputColumnIndex == k) &&
						(f->fAggFunction == ROWAGG_COUNT_DISTINCT_COL_NAME ||
						 f->fAggFunction == ROWAGG_DISTINCT_SUM ||
						 f->fAggFunction == ROWAGG_DISTINCT_AVG))
					{
						SP_ROWAGG_FUNC_t funct(
							new RowAggFunctionCol(
								f->fAggFunction,
								f->fStatsFunction,
								groupBySub.size()-1,
								f->fOutputColumnIndex,
								f->fAuxColumnIndex));
						functionSub2.push_back(funct);
					}
				}
			}

			// construct sub-aggregator
			SP_ROWAGG_UM_t subAgg(new RowAggregationSubDistinct(groupBySub, functionSub1, &jobInfo.rm, jobInfo.umMemLimit));

			// add to rowAggDist
			multiDistinctAggregator->addSubAggregator(subAgg, subRg, functionSub2);
		}

		// cover any non-distinct column functions
		{
			vector<SP_ROWAGG_FUNC_t> functionSub1 = functionNoDistVec;
			vector<SP_ROWAGG_FUNC_t> functionSub2;
			for (uint64_t k = 0; k < returnedColVec.size(); k++)
			{
				// search non-distinct functions in functionVec
				vector<SP_ROWAGG_FUNC_t>::iterator it = functionVecUm.begin();
				while (it != functionVecUm.end())
				{
					SP_ROWAGG_FUNC_t f = *it++;
					if ((f->fOutputColumnIndex == k) &&
						(f->fAggFunction == ROWAGG_COUNT_ASTERISK ||
						 f->fAggFunction == ROWAGG_COUNT_COL_NAME ||
						 f->fAggFunction == ROWAGG_SUM ||
						 f->fAggFunction == ROWAGG_AVG ||
						 f->fAggFunction == ROWAGG_MIN ||
						 f->fAggFunction == ROWAGG_MAX ||
						 f->fAggFunction == ROWAGG_STATS   ||
						 f->fAggFunction == ROWAGG_BIT_AND ||
						 f->fAggFunction == ROWAGG_BIT_OR  ||
						 f->fAggFunction == ROWAGG_BIT_XOR ||
						 f->fAggFunction == ROWAGG_CONSTANT))
					{
						SP_ROWAGG_FUNC_t funct(
							new RowAggFunctionCol(
								f->fAggFunction,
								f->fStatsFunction,
								f->fInputColumnIndex,
								f->fOutputColumnIndex,
								f->fAuxColumnIndex));
						functionSub2.push_back(funct);
					}
				}
			}

			if (functionSub1.size() > 0)
			{
				// make sure the group by columns are available for next aggregate phase.
				vector<SP_ROWAGG_GRPBY_t> groupBySubNoDist;
				for (uint64_t i = 0; i < groupByNoDist.size(); i++)
					groupBySubNoDist.push_back(SP_ROWAGG_GRPBY_t(
						new RowAggGroupByCol(groupByNoDist[i]->fInputColumnIndex, i)));

				// construct sub-aggregator
				SP_ROWAGG_UM_t subAgg(
					new RowAggregationUMP2(groupBySubNoDist, functionSub1, &jobInfo.rm, jobInfo.umMemLimit));

				// add to rowAggDist
				multiDistinctAggregator->addSubAggregator(subAgg, aggRgUm, functionSub2);
				subRgVec.push_back(aggRgUm);
			}
		}
	}

	rowAggDist->addAggregator(rowAggUm, aggRgUm);
	rowgroups.push_back(aggRgDist);
	aggregators.push_back(rowAggDist);

	posAggPm.push_back(2);   // rid
	for (uint64_t i = 0; i < oidsAggPm.size(); i++)
		posAggPm.push_back(posAggPm[i] + widthAggPm[i]);
	RowGroup aggRgPm(oidsAggPm.size(), posAggPm, oidsAggPm, keysAggPm, typeAggPm, scaleAggPm,
		precisionAggPm, jobInfo.stringTableThreshold);
	SP_ROWAGG_PM_t rowAggPm(new RowAggregation(groupByPm, functionVecPm));
	rowgroups.push_back(aggRgPm);
	aggregators.push_back(rowAggPm);

	if (jobInfo.trace) {
		cout << "projected   RG: " << projRG.toString() << endl
			 << "aggregated1 RG: " << aggRgPm.toString() << endl
			 << "aggregated2 RG: " << aggRgUm.toString() << endl;

		for (uint64_t i = 0; i < subRgVec.size(); i++)
		 	cout << "aggregatedSub RG: " << i << " " << subRgVec[i].toString() << endl;

	 	cout << "aggregatedDist RG: " << aggRgDist.toString() << endl;
	}
}


void TupleAggregateStep::prepExpressionOnAggregate(SP_ROWAGG_UM_t& aggUM, JobInfo& jobInfo)
{
	map<uint32_t, uint32_t> keyToIndexMap;
	for (uint64_t i = 0; i < fRowGroupOut.getKeys().size(); ++i)
	{
		if (keyToIndexMap.find(fRowGroupOut.getKeys()[i]) == keyToIndexMap.end())
			keyToIndexMap.insert(make_pair(fRowGroupOut.getKeys()[i], i));
	}

	RetColsVector expressionVec;
	ArithmeticColumn* ac = NULL;
	FunctionColumn* fc = NULL;
	RetColsVector& cols = jobInfo.nonConstCols;
	vector<SimpleColumn*> simpleColumns;
	for (RetColsVector::iterator it = cols.begin(); it != cols.end(); ++it)
	{
		uint64_t eid = -1;
		if (((ac = dynamic_cast<ArithmeticColumn*>(it->get())) != NULL) &&
				(ac->aggColumnList().size() > 0))
		{
			const vector<SimpleColumn*>& scols = ac->simpleColumnList();
			simpleColumns.insert(simpleColumns.end(), scols.begin(), scols.end());

			eid = ac->expressionId();
			expressionVec.push_back(*it);
		}
		else if (((fc = dynamic_cast<FunctionColumn*>(it->get())) != NULL) &&
				(fc->aggColumnList().size() > 0))
		{
			const vector<SimpleColumn*>& sCols = fc->simpleColumnList();
			simpleColumns.insert(simpleColumns.end(), sCols.begin(), sCols.end());

			eid = fc->expressionId();
			expressionVec.push_back(*it);
		}

		// update the output index
		if (eid != (uint64_t) -1)
		{
			map<uint32_t, uint32_t>::iterator mit = keyToIndexMap.find(getExpTupleKey(jobInfo, eid));

			if (mit != keyToIndexMap.end())
			{
				it->get()->outputIndex(mit->second);
			}
			else
			{
				ostringstream emsg;
				emsg << "expression " << eid << " cannot be found in tuple.";
				cerr << "prepExpressionOnAggregate: " << emsg.str() << endl;
				throw QueryDataExcept(emsg.str(), aggregateFuncErr);
			}
		}
	}

	// map the input indices
	for (vector<SimpleColumn*>::iterator i = simpleColumns.begin(); i != simpleColumns.end(); i++)
	{
		CalpontSystemCatalog::OID oid = (*i)->oid();
		uint32_t key = getTupleKey(jobInfo, *i);
		CalpontSystemCatalog::OID dictOid = joblist::isDictCol((*i)->colType());
		if (dictOid > 0)
		{
			oid = dictOid;
			key = jobInfo.keyInfo->dictKeyMap[key];
		}

		map<uint32_t, uint32_t>::iterator mit = keyToIndexMap.find(key);

		if (mit != keyToIndexMap.end())
		{
			(*i)->inputIndex(mit->second);
		}
		else
		{
			ostringstream emsg;
			emsg << "'" << jobInfo.keyInfo->tupleKeyToName[key] << "' cannot be found in tuple.";
			cerr << "prepExpressionOnAggregate: " << emsg.str() << "  simple column: oid("
				 << oid << "), alias(" << extractTableAlias(*i) << ")." << endl;
			throw QueryDataExcept(emsg.str(), aggregateFuncErr);
		}
	}

	// add expression to UM aggregator
	aggUM->expression(expressionVec);
}


void TupleAggregateStep::addConstangAggregate(vector<ConstantAggData>& constAggDataVec)
{
	fAggregator->constantAggregate(constAggDataVec);
}


void TupleAggregateStep::aggregateRowGroups()
{
	RGData rgData;
	bool more = true;
	RowGroupDL *dlIn = NULL;

	if (!fDoneAggregate)
	{
		if (fInputJobStepAssociation.outSize() == 0)
			throw logic_error("No input data list for TupleAggregate step.");

		dlIn = fInputJobStepAssociation.outAt(0)->rowGroupDL();
		if (dlIn == NULL)
			throw logic_error("Input is not RowGroup data list in TupleAggregate step.");

		if (fInputIter < 0)
			fInputIter = dlIn->getIterator();

		more = dlIn->next(fInputIter, &rgData);
		if (traceOn()) dlTimes.setFirstReadTime();

		StepTeleStats sts;
		sts.query_uuid = fQueryUuid;
		sts.step_uuid = fStepUuid;
		sts.msg_type = StepTeleStats::ST_START;
		sts.total_units_of_work = 1;
		postStepStartTele(sts);

		try
		{
			// this check covers the no row case
			if (!more && cancelled())
			{
				fDoneAggregate = true;
				fEndOfResult = true;
			}

			while (more && !fEndOfResult)
			{
				fRowGroupIn.setData(&rgData);
				fAggregator->addRowGroup(&fRowGroupIn);
				more = dlIn->next(fInputIter, &rgData);

				// error checking
				if (cancelled())
				{
					fEndOfResult = true;
					while (more)
						more = dlIn->next(fInputIter, &rgData);
				}
			}
		} // try
		catch (IDBExcept& iex)
		{
			catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
				(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
			fEndOfResult = true;
		}
		catch(const std::exception& ex)
		{
			catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);
			fEndOfResult = true;
		}
		catch(...)
		{
			catchHandler("TupleAggregateStep::aggregateRowGroups() caught an unknown exception",
							tupleAggregateStepErr, fErrorInfo, fSessionId);
			fEndOfResult = true;
		}
	}

	fDoneAggregate = true;

	while (more)
		more = dlIn->next(fInputIter, &rgData);

	if (traceOn())
	{
		dlTimes.setLastReadTime();
		dlTimes.setEndOfInputTime();
	}
}


void TupleAggregateStep::threadedAggregateRowGroups(uint32_t threadID)
{
	RGData rgData;
	scoped_array<RowBucketVec> rowBucketVecs(new RowBucketVec[fNumOfBuckets]);
	scoped_array<Row> distRow;
	scoped_array<shared_array<uint8_t> > distRowData;
	uint32_t bucketID;
	scoped_array<bool> bucketDone(new bool[fNumOfBuckets]);
	vector<uint32_t> hashLens;
	bool locked = false;
	bool more = true;
	RowGroupDL *dlIn = NULL;
	bool caughtException = false;

	RowAggregationMultiDistinct *multiDist = NULL;

	if (!fDoneAggregate)
	{
		if (fInputJobStepAssociation.outSize() == 0)
			throw logic_error("No input data list for delivery.");

		dlIn = fInputJobStepAssociation.outAt(0)->rowGroupDL();
		if (dlIn == NULL)
			throw logic_error("Input is not RowGroup data list in delivery step.");

		vector<RGData> rgDatas;

		try
		{
			// this check covers the no row case
			if (!more && cancelled())
			{
				fDoneAggregate = true;
				fEndOfResult = true;
			}

			bool firstRead = true;
			Row rowIn;
			while (more && !fEndOfResult)
			{
				fMutex.lock();
				locked = true;
				for (uint32_t c = 0; c < fNumOfRowGroups && !cancelled(); c++)
				{
					more = dlIn->next(fInputIter, &rgData);
					if (firstRead)
					{
						if (threadID == 0)
						{
							if (traceOn())
								dlTimes.setFirstReadTime();

							StepTeleStats sts;
							sts.query_uuid = fQueryUuid;
							sts.step_uuid = fStepUuid;
							sts.msg_type = StepTeleStats::ST_START;
							sts.total_units_of_work = 1;
							postStepStartTele(sts);
						}

						multiDist = dynamic_cast<RowAggregationMultiDistinct*>(fAggregator.get());
						if (multiDist)
						{
							for (uint32_t i = 0; i < fNumOfBuckets; i++)
								rowBucketVecs[i].resize(multiDist->subAggregators().size());

							distRow.reset(new Row[multiDist->subAggregators().size()]);
							distRowData.reset(new shared_array<uint8_t>[
													multiDist->subAggregators().size()]);
							for (uint32_t j = 0; j < multiDist->subAggregators().size(); j++)
							{
								multiDist->subAggregators()[j]->getOutputRowGroup()->initRow(
																	&distRow[j], true);
								distRowData[j].reset(new uint8_t[distRow[j].getSize()]);
								distRow[j].setData(distRowData[j].get());
								hashLens.push_back(multiDist->subAggregators()[j]->aggMapKeyLength());
							}
						}
						else
						{
							for (uint32_t i = 0; i < fNumOfBuckets; i++)
								rowBucketVecs[i].resize(1);

							if (dynamic_cast<RowAggregationDistinct*>(fAggregator.get()))
								hashLens.push_back(dynamic_cast<RowAggregationDistinct*>(fAggregator.get())->aggregator()->aggMapKeyLength());
							else
								hashLens.push_back(fAggregator->aggMapKeyLength());
						}

						fRowGroupIns[threadID] = fRowGroupIn;
						fRowGroupIns[threadID].initRow(&rowIn);
						firstRead = false;
					}

					if (more)
					{
					    // Trying out a ramp-up strategy for starting the
                        // first phase threads to cut overhead on big systems
                        // processing small result
                        // sets.  On every non-zero read from the input FIFO,
                        // and if there is more data to read, the
                        // first thread will start another thread until the
                        // maximum number is reached.
                        if (threadID == 0 && fFirstPhaseThreadCount < fNumOfThreads &&
                          dlIn->more(fInputIter)) {
                            fFirstPhaseRunners[fFirstPhaseThreadCount].reset
                                (new boost::thread(ThreadedAggregator(this, fFirstPhaseThreadCount)));
                            fFirstPhaseThreadCount++;
                        }

						fRowGroupIns[threadID].setData(&rgData);
						fMemUsage[threadID] += fRowGroupIns[threadID].getSizeWithStrings();
						if (!fRm.getMemory(fRowGroupIns[threadID].getSizeWithStrings(), fSessionMemLimit))
						{
							rgDatas.clear();    // to short-cut the rest of processing
							abort();
							more = false;
							fEndOfResult = true;
							if (status() == 0)
							{
								errorMessage(IDBErrorInfo::instance()->errorMsg(
									ERR_AGGREGATION_TOO_BIG));
								status(ERR_AGGREGATION_TOO_BIG);
							}
							break;
						}
						else
						{
							rgDatas.push_back(rgData);
						}
					}
					else
					{
						break;
					}
				}

				// input rowgroup and aggregator is finalized only right before hashjoin starts
				// if there is.
				if (fAggregators.empty())
				{
					fAggregators.resize(fNumOfBuckets);
					for (uint32_t i = 0; i < fNumOfBuckets; i++)
					{
						fAggregators[i].reset(fAggregator->clone());
						fAggregators[i]->setInputOutput(fRowGroupIn, &fRowGroupOuts[i]);
					}
				}
				fMutex.unlock();
				locked = false;

				multiDist = dynamic_cast<RowAggregationMultiDistinct*>(fAggregator.get());

				// dispatch rows to row buckets
				if (multiDist)
				{
					for (uint32_t c = 0; c < rgDatas.size(); c++)
					{
						fRowGroupIns[threadID].setData(&rgDatas[c]);
						for (uint32_t j = 0; j < multiDist->subAggregators().size(); j++)
						{
							fRowGroupIns[threadID].getRow(0, &rowIn);
							for (uint64_t i = 0; i < fRowGroupIns[threadID].getRowCount(); ++i)
							{
								for (uint64_t k = 0;
									 k < multiDist->subAggregators()[j]->getGroupByCols().size();
									 ++k)
								{
									rowIn.copyField(distRow[j], k, multiDist->subAggregators()[j]->getGroupByCols()[k].get()->fInputColumnIndex);
								}
								bucketID = distRow[j].hash(hashLens[j] - 1) % fNumOfBuckets;
								rowBucketVecs[bucketID][j].push_back(rowIn.getPointer());
								rowIn.nextRow();
							}
						}
					}
				}
				else
				{
					for (uint32_t c = 0; c < rgDatas.size(); c++)
					{
						fRowGroupIns[threadID].setData(&rgDatas[c]);
						fRowGroupIns[threadID].getRow(0, &rowIn);
						for (uint64_t i = 0; i < fRowGroupIns[threadID].getRowCount(); ++i)
						{
							// The key is the groupby columns, which are the leading columns.
							int bucketID = rowIn.hash(hashLens[0] - 1) % fNumOfBuckets;
							rowBucketVecs[bucketID][0].push_back(rowIn.getPointer());
							rowIn.nextRow();
						}
					}
				}

				// insert to the hashmaps owned by each aggregator
				bool done = false;
				fill(&bucketDone[0], &bucketDone[fNumOfBuckets], false);
				while (!fEndOfResult && !done && !cancelled())
				{
					bool didWork = false;
					done = true;
					for (uint32_t c = 0; c < fNumOfBuckets && !cancelled(); c++)
					{
						if (!fEndOfResult && !bucketDone[c] && fAgg_mutex[c]->try_lock())
						{
							try
							{
								didWork = true;
								if (multiDist)
									dynamic_cast<RowAggregationMultiDistinct*>(fAggregators[c].get())->addRowGroup(&fRowGroupIns[threadID], rowBucketVecs[c]);
								else
									fAggregators[c]->addRowGroup(&fRowGroupIns[threadID], rowBucketVecs[c][0]);
							}
							catch(...)
							{
								fAgg_mutex[c]->unlock();
								throw;
							}
							fAgg_mutex[c]->unlock();
							rowBucketVecs[c][0].clear();
							bucketDone[c] = true;
						}
						else if (!bucketDone[c])
						{
							done = false;
						}
					}
					if (!didWork)
						usleep(1000);   // avoid using all CPU during busy wait
				}
				rgDatas.clear();
				fRm.returnMemory(fMemUsage[threadID], fSessionMemLimit);
				fMemUsage[threadID] = 0;

				if (cancelled())
				{
					fEndOfResult = true;
					fMutex.lock();

					while (more)
						more = dlIn->next(fInputIter, &rgData);

					fMutex.unlock();
				}
			}
		} // try
		catch (IDBExcept& iex)
		{
			catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
				(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
			caughtException = true;
		}
		catch(const std::exception& ex)
		{
			catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);
			caughtException = true;
		}
		catch(...)
		{
			catchHandler("threadedAggregateRowGroups() caught an unknown exception",
							tupleAggregateStepErr, fErrorInfo, fSessionId);
			caughtException = true;
		}
	}

	if (caughtException)
	{
		fEndOfResult = true;
		fDoneAggregate = true;
	}

	if (!locked) fMutex.lock();
	while (more) more = dlIn->next(fInputIter, &rgData);
	fMutex.unlock();
	locked = false;

	if (traceOn())
	{
		dlTimes.setLastReadTime();
		dlTimes.setEndOfInputTime();
	}
}


void TupleAggregateStep::doAggregate_singleThread()
{
	AnyDataListSPtr dl = fOutputJobStepAssociation.outAt(0);
	RowGroupDL* dlp = dl->rowGroupDL();
	RGData rgData;

	try
	{
		if (!fDoneAggregate)
			aggregateRowGroups();

		if (fEndOfResult == false)
		{
			// do the final aggregtion and deliver the results
			// at least one RowGroup for aggregate results
			if (dynamic_cast<RowAggregationDistinct*>(fAggregator.get()) != NULL)
			{
				dynamic_cast<RowAggregationDistinct*>(fAggregator.get())->doDistinctAggregation();
			}

			while (fAggregator->nextRowGroup())
			{
				fAggregator->finalize();
				fRowsReturned += fRowGroupOut.getRowCount();
				rgData = fRowGroupOut.duplicate();
				fRowGroupDelivered.setData(&rgData);
				if (fRowGroupOut.getColumnCount() > fRowGroupDelivered.getColumnCount())
					pruneAuxColumns();
				dlp->insert(rgData);
			}
		}
	} // try
	catch (IDBExcept& iex)
	{
		catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
			(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
	}
	catch(const std::exception& ex)
	{
		catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);
	}
	catch(...)
	{
		catchHandler("TupleAggregateStep next band caught an unknown exception",
						tupleAggregateStepErr, fErrorInfo, fSessionId);
	}

	if (traceOn())
		printCalTrace();

	StepTeleStats sts;
	sts.query_uuid = fQueryUuid;
	sts.step_uuid = fStepUuid;
	sts.msg_type = StepTeleStats::ST_SUMMARY;
	sts.total_units_of_work = sts.units_of_work_completed = 1;
	sts.rows = fRowsReturned;
	postStepSummaryTele(sts);

	// Bug 3136, let mini stats to be formatted if traceOn.
	fEndOfResult = true;
	dlp->endOfInput();
}


void TupleAggregateStep::doAggregate()
{
	// @bug4314. DO NOT access fAggregtor before the first read of input,
	// because hashjoin may not have finalized fAggregator.
	if (!fIsMultiThread)
		return doAggregate_singleThread();

	AnyDataListSPtr dl = fOutputJobStepAssociation.outAt(0);
	RowGroupDL* dlp = dl->rowGroupDL();
	ByteStream bs;
	doThreadedAggregate(bs, dlp);
	return;
}


uint64_t TupleAggregateStep::doThreadedAggregate(ByteStream& bs, RowGroupDL* dlp)
{
	uint32_t i;
	RGData rgData;
	uint64_t rowCount = 0;

	try {
		if (!fDoneAggregate)
		{
			initializeMultiThread();
/*
//          This block of code starts all threads at the start
          fFirstPhaseThreadCount = fNumOfThreads;
          boost::shared_ptr<boost::thread> runner;
          for (i = 0; i < fNumOfThreads; i++)
          {
              runner.reset(new boost::thread(ThreadedAggregator(this, i)));
              fFirstPhaseRunners.push_back(runner);
          }
*/

//          This block of code starts one thread, relies on doThreadedAggregation()
//          to start more as needed
            fFirstPhaseRunners.resize(fNumOfThreads); // to prevent a resize during use
            fFirstPhaseThreadCount = 1;
            for (i = 1; i < fNumOfThreads; i++)
                // fill with valid thread objects to make joining work
                fFirstPhaseRunners[i].reset(new boost::thread());
            fFirstPhaseRunners[0].reset(new boost::thread(ThreadedAggregator(this, 0)));

            for (i = 0; i < fNumOfThreads; i++)
                fFirstPhaseRunners[i]->join();
            fFirstPhaseRunners.clear();
        }

		if (dynamic_cast<RowAggregationDistinct*>(fAggregator.get()) && fAggregator->aggMapKeyLength() > 0)
		{
			// 2nd phase multi-threaded aggregate
			if (!fEndOfResult)
			{
				if (!fDoneAggregate)
				{
					vector<boost::shared_ptr<thread> > runners;
					boost::shared_ptr<thread> runner;
					fRowGroupsDeliveredData.resize(fNumOfBuckets);

                    uint32_t bucketsPerThread = fNumOfBuckets/fNumOfThreads;
                    uint32_t numThreads = ((fNumOfBuckets % fNumOfThreads) == 0 ?
                        fNumOfThreads : fNumOfThreads + 1);
                    //uint32_t bucketsPerThread = 1;
                    //uint32_t numThreads = fNumOfBuckets;

                    for (i = 0; i < numThreads; i++)
                    {
                        runner.reset(new boost::thread(ThreadedSecondPhaseAggregator(this, i*bucketsPerThread, bucketsPerThread)));
                        runners.push_back(runner);
                    }
                    for (i = 0; i < numThreads; i++)
                        runners[i]->join();
                }

				fDoneAggregate = true;
				bool done = true;
				while (nextDeliveredRowGroup())
				{
					done = false;
					rowCount = fRowGroupOut.getRowCount();
					if ( rowCount != 0 )
					{
						if (fRowGroupOut.getColumnCount() != fRowGroupDelivered.getColumnCount())
							pruneAuxColumns();

						if (dlp)
						{
							rgData = fRowGroupDelivered.duplicate();
							dlp->insert(rgData);
						}
						else
						{
							bs.restart();
							fRowGroupDelivered.serializeRGData(bs);
							break;
						}
					}
					done = true;
				}

				if (done)
					fEndOfResult = true;
				}
		}
		else
		{
			RowAggregationDistinct* agg = dynamic_cast<RowAggregationDistinct*>(fAggregator.get());
			if (!fEndOfResult)
			{
				if (!fDoneAggregate)
				{
					for (i = 0; i < fNumOfBuckets; i++)
					{
						if (fEndOfResult == false)
						{
							// do the final aggregtion and deliver the results
							// at least one RowGroup for aggregate results
							// for "distinct without group by" case
							if (agg != NULL)
							{
								RowAggregationMultiDistinct *aggMultiDist =
								dynamic_cast<RowAggregationMultiDistinct*>(fAggregators[i].get());
								RowAggregationDistinct *aggDist =
								dynamic_cast<RowAggregationDistinct*>(fAggregators[i].get());
								agg->aggregator(aggDist->aggregator());
								if (aggMultiDist)
									(dynamic_cast<RowAggregationMultiDistinct*>(agg))
										->subAggregators(aggMultiDist->subAggregators());
								agg->doDistinctAggregation();
							}
							// for "group by without distinct" case
							else
							{
								fAggregator->resultDataVec().insert(
									fAggregator->resultDataVec().end(),
									fAggregators[i]->resultDataVec().begin(),
									fAggregators[i]->resultDataVec().end());
							}
						}
					}
				}

				fDoneAggregate = true;
			}

			bool done = true;
			//@bug4459
			while (fAggregator->nextRowGroup() && !cancelled())
			{
				done = false;
				fAggregator->finalize();
				rowCount = fRowGroupOut.getRowCount();
				fRowsReturned += rowCount;
				fRowGroupDelivered.setData(fRowGroupOut.getRGData());

				if (rowCount != 0)
				{
					if (fRowGroupOut.getColumnCount() != fRowGroupDelivered.getColumnCount())
						pruneAuxColumns();

					if (dlp)
					{
						rgData = fRowGroupDelivered.duplicate();
						dlp->insert(rgData);
					}
					else
					{
						bs.restart();
						fRowGroupDelivered.serializeRGData(bs);
						break;
					}
				}
				done = true;
			}

			if (done)
			{
				fEndOfResult = true;
			}
		}
	}
	catch (IDBExcept& iex)
	{
		catchHandler(iex.what(), iex.errorCode(), fErrorInfo, fSessionId,
			(iex.errorCode() == ERR_AGGREGATION_TOO_BIG ? LOG_TYPE_INFO : LOG_TYPE_CRITICAL));
		fEndOfResult = true;
	}
	catch(const std::exception& ex)
	{
		catchHandler(ex.what(), tupleAggregateStepErr, fErrorInfo, fSessionId);
		fEndOfResult = true;
	}

	if (fEndOfResult)
	{
		StepTeleStats sts;
		sts.query_uuid = fQueryUuid;
		sts.step_uuid = fStepUuid;
		sts.msg_type = StepTeleStats::ST_SUMMARY;
		sts.total_units_of_work = sts.units_of_work_completed = 1;
		sts.rows = fRowsReturned;
		postStepSummaryTele(sts);

		if (dlp)
		{
			dlp->endOfInput();
		}
		else
		{
			// send an empty / error band
			RGData rgData(fRowGroupOut, 0);
			fRowGroupOut.setData(&rgData);
			fRowGroupOut.resetRowGroup(0);
			fRowGroupOut.setStatus(status());
			fRowGroupOut.serializeRGData(bs);
			rowCount = 0;
		}

		if (traceOn())
			printCalTrace();
	}

	return rowCount;
}


void TupleAggregateStep::pruneAuxColumns()
{
	uint64_t rowCount = fRowGroupOut.getRowCount();
	Row row1, row2;
	fRowGroupOut.initRow(&row1);
	fRowGroupOut.getRow(0, &row1);
	fRowGroupDelivered.initRow(&row2);
	fRowGroupDelivered.getRow(0, &row2);
	for (uint64_t i = 1; i < rowCount; i++)
	{
		// skip the first row
		row1.nextRow();
		row2.nextRow();

		// bug4463, memmove for src, dest overlap
		memmove(row2.getData(), row1.getData(), row2.getSize());
	}
}


void TupleAggregateStep::printCalTrace()
{
	time_t t = time (0);
	char timeString[50];
	ctime_r (&t, timeString);
	timeString[strlen (timeString )-1] = '\0';
	ostringstream logStr;
	logStr  << "ses:" << fSessionId << " st: " << fStepId << " finished at "<< timeString
			<< "; total rows returned-" << fRowsReturned << endl
			<< "\t1st read " << dlTimes.FirstReadTimeString()
			<< "; EOI " << dlTimes.EndOfInputTimeString() << "; runtime-"
			<< JSTimeStamp::tsdiffstr(dlTimes.EndOfInputTime(), dlTimes.FirstReadTime())
			<< "s;\n\tUUID " << uuids::to_string(fStepUuid) << endl
			<< "\tJob completion status " << status() << endl;
	logEnd(logStr.str().c_str());
	fExtendedInfo += logStr.str();
	formatMiniStats();
}


void TupleAggregateStep::formatMiniStats()
{
	ostringstream oss;
	oss << "TAS "
		<< "UM "
		<< "- "
		<< "- "
		<< "- "
		<< "- "
		<< "- "
		<< "- "
		<< JSTimeStamp::tsdiffstr(dlTimes.EndOfInputTime(), dlTimes.FirstReadTime()) << " "
		<< fRowsReturned << " ";
	fMiniInfo += oss.str();
}


}   //namespace
// vim:ts=4 sw=4:

