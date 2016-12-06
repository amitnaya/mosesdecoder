/*
 * HypothesisColl.cpp
 *
 *  Created on: 26 Feb 2016
 *      Author: hieu
 */
#include <iostream>
#include <sstream>
#include <algorithm>
#include <boost/foreach.hpp>
#include "HypothesisColl.h"
#include "ManagerBase.h"
#include "System.h"
#include "MemPoolAllocator.h"

using namespace std;

namespace Moses2
{

HypothesisColl::HypothesisColl(const ManagerBase &mgr)
:m_coll(MemPoolAllocator<const HypothesisBase*>(mgr.GetPool()))
,m_sortedHypos(NULL)
{
  m_bestScore = -std::numeric_limits<float>::infinity();
  m_worstScore = std::numeric_limits<float>::infinity();
}

const HypothesisBase *HypothesisColl::GetBestHypo() const
{
	if (GetSize() == 0) {
		return NULL;
	}
	if (m_sortedHypos) {
		return (*m_sortedHypos)[0];
	}

	SCORE bestScore = -std::numeric_limits<SCORE>::infinity();
	const HypothesisBase *bestHypo;
	BOOST_FOREACH(const HypothesisBase *hypo, m_coll) {
		if (hypo->GetFutureScore() > bestScore) {
			bestScore = hypo->GetFutureScore();
			bestHypo = hypo;
		}
	}
	return bestHypo;
}

void HypothesisColl::Add(
    const ManagerBase &mgr,
		HypothesisBase *hypo,
		Recycler<HypothesisBase*> &hypoRecycle,
		ArcLists &arcLists)
{
  size_t maxStackSize = mgr.system.options.search.stack_size;

  if (GetSize() > maxStackSize * 2) {
    //cerr << "maxStackSize=" << maxStackSize << " " << GetSize() << endl;
    const HypothesisBase *sortedHypos[GetSize()];
    PruneHypos(mgr, mgr.arcLists, sortedHypos);
  }

  SCORE futureScore = hypo->GetFutureScore();
  /*
  cerr << "scores:"
      << futureScore << " "
      << m_bestScore << " "
      << m_minBeamScore << " "
      << GetSize() << " "
      << endl;
  */
  if (GetSize() >= maxStackSize && futureScore < m_worstScore) {
    // beam threshold or really bad hypo that won't make the pruning cut
    // as more hypos are added, the m_worstScore stat gets out of date and isn't the optimum cut-off point
    //cerr << "Discard, really bad score:" << hypo->Debug(system) << endl;
    hypoRecycle.Recycle(hypo);
    return;
  }

	StackAdd added = Add(hypo);

	size_t nbestSize = mgr.system.options.nbest.nbest_size;
	if (nbestSize) {
		arcLists.AddArc(added.added, hypo, added.other);
	}
	else {
		if (added.added) {
      if (added.other) {
        hypoRecycle.Recycle(added.other);
      }
		}
		else {
      hypoRecycle.Recycle(hypo);
		}
	}

  // update beam variables
	if (added.added) {
    if (futureScore > m_bestScore) {
      m_bestScore = futureScore;
      float beamWidth = mgr.system.options.search.beam_width;
      if ( m_bestScore + beamWidth > m_worstScore ) {
        m_worstScore = m_bestScore + beamWidth;
      }
    }
    else if (GetSize() <= maxStackSize && futureScore < m_worstScore) {
      m_worstScore = futureScore;
    }
	}
}

StackAdd HypothesisColl::Add(const HypothesisBase *hypo)
{
	std::pair<_HCType::iterator, bool> addRet = m_coll.insert(hypo);

	// CHECK RECOMBINATION
	if (addRet.second) {
		// equiv hypo doesn't exists
		return StackAdd(true, NULL);
	}
	else {
		HypothesisBase *hypoExisting = const_cast<HypothesisBase*>(*addRet.first);
		if (hypo->GetFutureScore() > hypoExisting->GetFutureScore()) {
			// incoming hypo is better than the one we have
			const HypothesisBase * const &hypoExisting1 = *addRet.first;
			const HypothesisBase *&hypoExisting2 =
					const_cast<const HypothesisBase *&>(hypoExisting1);
			hypoExisting2 = hypo;

			return StackAdd(true, hypoExisting);
		}
		else {
			// already storing the best hypo. discard incoming hypo
			return StackAdd(false, hypoExisting);
		}
	}

	//assert(false);
}

const Hypotheses &HypothesisColl::GetSortedAndPruneHypos(
		const ManagerBase &mgr,
		ArcLists &arcLists) const
{
	if (m_sortedHypos == NULL) {
		// create sortedHypos first
		MemPool &pool = mgr.GetPool();
		m_sortedHypos = new (pool.Allocate<Hypotheses>()) Hypotheses(pool,
				m_coll.size());

		size_t ind = 0;
		BOOST_FOREACH(const HypothesisBase *hypo, m_coll){
			(*m_sortedHypos)[ind] = hypo;
			++ind;
		}

		SortAndPruneHypos(mgr, arcLists);
	}

	return *m_sortedHypos;
}

const Hypotheses &HypothesisColl::GetSortedAndPrunedHypos() const
{
	UTIL_THROW_IF2(m_sortedHypos == NULL, "m_sortedHypos must be sorted beforehand");
	return *m_sortedHypos;
}

void HypothesisColl::SortAndPruneHypos(const ManagerBase &mgr,
		ArcLists &arcLists) const
{
	size_t stackSize = mgr.system.options.search.stack_size;
	Recycler<HypothesisBase*> &recycler = mgr.GetHypoRecycle();

	/*
   cerr << "UNSORTED hypos: ";
   BOOST_FOREACH(const HypothesisBase *hypo, m_coll) {
	   cerr << hypo << "(" << hypo->GetFutureScore() << ")" << " ";
   }
   cerr << endl;
	 */
	Hypotheses::iterator iterMiddle;
	iterMiddle =
			(stackSize == 0 || m_sortedHypos->size() < stackSize) ?
					m_sortedHypos->end() : m_sortedHypos->begin() + stackSize;

	std::partial_sort(m_sortedHypos->begin(), iterMiddle, m_sortedHypos->end(),
			HypothesisFutureScoreOrderer());

	// prune
	if (stackSize && m_sortedHypos->size() > stackSize) {
		for (size_t i = stackSize; i < m_sortedHypos->size(); ++i) {
			HypothesisBase *hypo = const_cast<HypothesisBase*>((*m_sortedHypos)[i]);
			recycler.Recycle(hypo);

			// delete from arclist
			if (mgr.system.options.nbest.nbest_size) {
				arcLists.Delete(hypo);
			}
		}
		m_sortedHypos->resize(stackSize);
	}

	/*
   cerr << "sorted hypos: ";
   for (size_t i = 0; i < m_sortedHypos->size(); ++i) {
   const HypothesisBase *hypo = (*m_sortedHypos)[i];
   	   cerr << hypo << " ";
   }
   cerr << endl;
	 */
}

void HypothesisColl::PruneHypos(const ManagerBase &mgr, ArcLists &arcLists, const HypothesisBase **sortedHypos)
{
  size_t maxStackSize = mgr.system.options.search.stack_size;
  assert(maxStackSize); // can't do stack=0 - unlimited stack size. No-one ever uses that
  assert(GetSize() > maxStackSize);
  //assert(sortedHypos.size() == GetSize());

  Recycler<HypothesisBase*> &recycler = mgr.GetHypoRecycle();

  /*
   cerr << "UNSORTED hypos: ";
   BOOST_FOREACH(const HypothesisBase *hypo, m_coll) {
     cerr << hypo << "(" << hypo->GetFutureScore() << ")" << " ";
   }
   cerr << endl;
   */
  size_t ind = 0;
  BOOST_FOREACH(const HypothesisBase *hypo, m_coll){
    sortedHypos[ind] = hypo;
    ++ind;
  }

  const HypothesisBase **iterMiddle = sortedHypos + maxStackSize;

  std::partial_sort(
      sortedHypos,
      iterMiddle,
      sortedHypos + GetSize(),
      HypothesisFutureScoreOrderer());

  // update worse score
  m_worstScore = sortedHypos[maxStackSize - 1]->GetFutureScore();

  // prune
  for (size_t i = maxStackSize; i < GetSize(); ++i) {
    HypothesisBase *hypo = const_cast<HypothesisBase*>(sortedHypos[i]);

    // delete from arclist
    if (mgr.system.options.nbest.nbest_size) {
      arcLists.Delete(hypo);
    }

    // delete from collection
    Delete(hypo);

    recycler.Recycle(hypo);
  }

  /*
   cerr << "sorted hypos: ";
   for (size_t i = 0; i < sortedHypos.size(); ++i) {
     const HypothesisBase *hypo = sortedHypos[i];
     cerr << hypo << " ";
   }
   cerr << endl;
   */
}

void HypothesisColl::Delete(const HypothesisBase *hypo)
{
  //cerr << "hypo=" << hypo << " " << m_coll.size() << endl;

  size_t erased = m_coll.erase(hypo);
  UTIL_THROW_IF2(erased != 1, "couldn't erase hypo " << hypo);
}

void HypothesisColl::Clear()
{
	m_sortedHypos = NULL;
	m_coll.clear();

  m_bestScore = -std::numeric_limits<float>::infinity();
  m_worstScore = std::numeric_limits<float>::infinity();
}

std::string HypothesisColl::Debug(const System &system) const
{
	stringstream out;
	BOOST_FOREACH (const HypothesisBase *hypo, m_coll) {
		out << hypo->Debug(system);
		out << std::endl << std::endl;
	}

	return out.str();
}

} /* namespace Moses2 */
