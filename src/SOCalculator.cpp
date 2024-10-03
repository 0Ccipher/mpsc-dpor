/*
* Omkar Tuppe
 */

#include "SOCalculator.hpp"
#include "ExecutionGraph.hpp"
#include "GraphIterators.hpp"
#include <vector>

SOCalculator::const_send_iterator
SOCalculator::succ_begin(Channel ch, Event send) const
{
	auto offset = getSendOffset(ch, send);
	return send_begin(ch) + (offset + 1);
}

SOCalculator::const_send_iterator
SOCalculator::succ_end(Channel ch, Event send) const
{
	return send_end(ch);
}

SOCalculator::const_send_iterator
SOCalculator::pred_begin(Channel ch, Event send) const
{
	return send_begin(ch);
}

SOCalculator::const_send_iterator
SOCalculator::pred_end(Channel ch, Event send) const
{
	auto offset = getSendOffset(ch, send);
	return send_begin(ch) + (offset >= 0 ? offset : 0);
}

void SOCalculator::trackSendOrderAtCh(Channel ch)
{
	sends[ch];
}

int SOCalculator::getSendOffset(Channel ch, Event e) const
{
	BUG_ON(sends.count(ch) == 0);

	if (e == Event::getInitializer())
		return -1;

	auto oIt = std::find(send_begin(ch), send_end(ch), e);
	BUG_ON(oIt == send_end(ch));
	return std::distance(send_begin(ch), oIt);
}

std::pair<int, int>
SOCalculator::getPossiblePlacings(Channel ch, Event send)
{
	const auto &g = getGraph();

	auto rangeBegin = splitChSOBefore(ch, send);
	auto rangeEnd = (supportsOutOfOrder()) ? splitChSOAfter(ch, send) :
		getSendsToCh(ch).size();
	return std::make_pair(rangeBegin, rangeEnd);

}

void SOCalculator::addSendToCh(Channel ch, Event send, int offset)
{
	if (offset == -1)
		sends[ch].push_back(send);
	else
		sends[ch].insert(send_begin(ch) + offset, send);
}

void SOCalculator::addSendToChAfter(Channel ch, Event send, Event pred)
{
	int offset = getSendOffset(ch, pred);
	addSendToCh(ch, send, offset + 1);
}

bool SOCalculator::isCoMaximal(Channel ch, Event send)
{
	auto &chSO = sends[ch];
	return (send.isInitializer() && chSO.empty()) ||
	       (!send.isInitializer() && !chSO.empty() && send == chSO.back());
}

bool SOCalculator::isCachedCoMaximal(Channel ch, Event send)
{
	return isCoMaximal(ch, send);
}

void SOCalculator::changeSendOffset(Channel ch, Event send, int newOffset)
{
	auto &chSO = sends[ch];

	chSO.erase(std::find(send_begin(ch), send_end(ch), send));
	chSO.insert(send_begin(ch) + newOffset, send);
}

int SOCalculator::splitChSOBefore(Channel ch, Event e)
{
	const auto &g = getGraph();
	auto rit = std::find_if(send_rbegin(ch), send_rend(ch), [&](const Event &s){
		return g.isWriteRfBefore(s, e.prev());
	});
	return (rit == send_rend(ch)) ? 0 : std::distance(rit, send_rend(ch));
}

int SOCalculator::splitChSOAfterHb(Channel ch, const Event receive)
{
	const auto &g = getGraph();

	// auto initRfs = g.getInitRfsAtCh(ch);
	// if (std::any_of(initRfs.begin(), initRfs.end(), [&receive,&g](const Event &rf){
	// 	return g.getEventLabel(rf)->getHbView().contains(receive);
	// }))
	// 	return 0;

	auto it = std::find_if(send_begin(ch), send_end(ch), [&](const Event &s){
		return g.isHbOptRfBefore(receive, s);
	});
	if (it == send_end(ch))
		return std::distance(send_begin(ch), send_end(ch));
	return (g.getEventLabel(*it)->getHbView().contains(receive)) ?
		std::distance(send_begin(ch), it) : std::distance(send_begin(ch), it) + 1;
}

int SOCalculator::splitChSOAfter(Channel ch, const Event e)
{
	const auto &g = getGraph();
	auto it = std::find_if(send_begin(ch), send_end(ch), [&](const Event &s){
		return g.isHbOptRfBefore(e, s);
	});
	return (it == send_end(ch)) ? std::distance(send_begin(ch), send_end(ch)) :
		std::distance(send_begin(ch), it);
}

std::vector<Event>
SOCalculator::getCoherentSends(Channel ch, Event receive)
{
	auto &g = getGraph();
	std::vector<Event> sends;

	/*
	 * If there are no sends (rf?;hb)-before the current event
	 * then we can receive receive from all concurrent sends and the
	 * initializer send. Otherwise, we can receive from all concurrent
	 * sends and the mo-latest of the (rf?;hb)-before sends.
	 */
	auto begO = splitChSOBefore(ch, receive);
	if (begO == 0)
		sends.push_back(Event::getInitializer());
	else
		sends.push_back(*(send_begin(ch) + begO - 1));

	/*
	 * If the model supports out-of-order execution we have to also
	 * account for the possibility the receive is hb-before some other
	 * send, or some receive that receives from a send.
	 */
	auto endO = (supportsOutOfOrder()) ? splitChSOAfterHb(ch, receive) :
		std::distance(send_begin(ch), send_end(ch));
	sends.insert(sends.end(), send_begin(ch) + begO, send_begin(ch) + endO);
	return sends;
}

std::vector<Event>
SOCalculator::getSOOptRfAfter(const SendLabel *sLab)
{
	std::vector<Event> after;

	std::for_each(succ_begin(sLab->getChannel(), sLab->getPos()),
		      succ_end(sLab->getChannel(), sLab->getPos()), [&](const Event &w){
			      auto *sendLab = getGraph().getSendLabel(w);
			      after.push_back(sendLab->getPos());
				if(!sendLab->getReader().isBottom())
			      	after.insert(after.end(), sendLab->getReader());
	});
	return after;
}

std::vector<Event>
SOCalculator::getSOInvOptRfAfter(const SendLabel *sLab)
{
	std::vector<Event> after;

	/* First, add (mo;rf?)-before */
	std::for_each(pred_begin(sLab->getChannel(), sLab->getPos()),
		      pred_end(sLab->getChannel(), sLab->getPos()), [&](const Event &w){
			      auto *sendLab = getGraph().getSendLabel(w);
			      after.push_back(sendLab->getPos());
			      if(!sendLab->getReader().isBottom())
			      	after.insert(after.end(), sendLab->getReader());
	});

	// /* Then, we add the receiveer list for the initializer */
	// auto initRfs = getGraph().getInitRfsAtCh(sLab->getChannel());
	// after.insert(after.end(), initRfs.begin(), initRfs.end());
	return after;
}

std::vector<Event>
SOCalculator::getCoherentRevisits(const SendLabel *sLab)
{
	const auto &g = getGraph();
	auto ls = g.getRevisitable(sLab);

	/* If this send is po- and mo-maximal then we are done */
	if (!supportsOutOfOrder() && isCoMaximal(sLab->getChannel(), sLab->getPos()))
		return ls;

	/* First, we have to exclude (mo;rf?;hb?;sb)-after receives */
	auto optRfs = getSOOptRfAfter(sLab);
	ls.erase(std::remove_if(ls.begin(), ls.end(), [&](Event e)
				{ const View &before = g.getHbPoBefore(e);
				  return std::any_of(optRfs.begin(), optRfs.end(),
					 [&](Event ev)
					 { return before.contains(ev); });
				}), ls.end());

	/* If out-of-order event addition is not supported, then we are done
	 * due to po-maximality */
	if (!supportsOutOfOrder())
		return ls;

	/* Otherwise, we also have to exclude hb-before loads */
	ls.erase(std::remove_if(ls.begin(), ls.end(), [&](Event e)
		{ return g.getEventLabel(sLab->getPos())->getHbView().contains(e); }),
		ls.end());

	/* ...and also exclude (mo^-1; rf?; (hb^-1)?; sb^-1)-after receives in
	 * the resulting graph */
	auto &before = g.getPPoRfBefore(sLab->getPos());
	auto moInvOptRfs = getSOInvOptRfAfter(sLab);
	ls.erase(std::remove_if(ls.begin(), ls.end(), [&](Event e)
				{ auto *eLab = g.getEventLabel(e);
				  auto v = g.getDepViewFromStamp(eLab->getStamp());
				  v.update(before);
				  return std::any_of(moInvOptRfs.begin(),
						     moInvOptRfs.end(),
						     [&](Event ev)
						     { return v.contains(ev) &&
						       g.getHbPoBefore(ev).contains(e); });
				}),
		 ls.end());

	return ls;
}


// bool SOCalculator::isCoBeforeSavedPrefix(const BackwardRevisit &r, const EventLabel *lab)
// {
// 	auto *mLab = llvm::dyn_cast<ChannelAccessLabel>(lab);
// 	if (!mLab)
// 		return false;

// 	auto &g = getGraph();
//         auto v = g.getRevisitView(r);
// 	auto w = llvm::isa<ReadLabel>(mLab) ? llvm::dyn_cast<ReadLabel>(mLab)->getRf() : mLab->getPos();
// 	return any_of(succ_begin(mLab->getChannel(), w),
// 		      succ_end(mLab->getChannel(), w), [&](const Event &s){
// 			      auto *sLab = g.getEventLabel(s);
// 			      return v->contains(sLab->getPos()) &&
// 				     mLab->getIndex() > sLab->getPPoRfView()[mLab->getThread()] &&
// 				     sLab->getPos() != r.getRev();
// 		      });
// }

// bool SOCalculator::coherenceSuccRemainInGraph(const BackwardRevisit &r)
// {
// 	auto &g = getGraph();
// 	auto *sendLab = g.getSendLabel(r.getRev());
// 	if (g.isRMWSend(sendLab))
// 		return true;

// 	auto succIt = succ_begin(sendLab->getChannel(), sendLab->getPos());
// 	auto succE = succ_end(sendLab->getChannel(), sendLab->getPos());
// 	if (succIt == succE)
// 		return true;

// 	return g.getRevisitView(r)->contains(*succIt);
// }

bool SOCalculator::wasAddedMaximally(const EventLabel *lab)
{
	if (auto *mLab = llvm::dyn_cast<ChannelAccessLabel>(lab))
		return mLab->wasAddedMax();
	if (auto *oLab = llvm::dyn_cast<OptionalLabel>(lab))
		return !oLab->isExpanded();
	return true;
}


void SOCalculator::initCalc()
{
	auto &gm = getGraph();
	auto &coRelation = gm.getPerChRelation(ExecutionGraph::RelationId::so);

	coRelation.clear();
	for (auto chIt = begin(); chIt != end(); chIt++) {
		coRelation[chIt->first] = GlobalRelation(getSendsToCh(chIt->first));
		if (chIt->second.empty())
			continue;
		for (auto sIt = chIt->second.begin(); sIt != chIt->second.end() - 1; sIt++)
			coRelation[chIt->first].addEdge(*sIt, *(sIt + 1));
		coRelation[chIt->first].transClosure();
	}
	return;
}

Calculator::CalculationResult SOCalculator::doCalc()
{
	auto &gm = getGraph();
	auto &coRelation = gm.getPerChRelation(ExecutionGraph::RelationId::so);

	for (auto chIt = begin(); chIt != end(); chIt++) {
		if (!coRelation[chIt->first].isIrreflexive())
			return Calculator::CalculationResult(false, false);
	}
	return Calculator::CalculationResult(false, true);
}

void SOCalculator::removeAfter(const VectorClock &preds)
{
	auto &g = getGraph();
	VSet<Channel> keep;

	/* Check which chations should be kept */
	for (auto i = 0u; i < preds.size(); i++) {
		for (auto j = 0u; j <= preds[i]; j++) {
			auto *lab = g.getEventLabel(Event(i, j));
			if (auto *mLab = llvm::dyn_cast<ChannelAccessLabel>(lab))
				keep.insert(mLab->getChannel());
		}
	}

	for (auto it = begin(); it != end(); /* empty */) {
		it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
						[&](Event &e)
						{ return !preds.contains(e); }),
				 it->second.end());

		/* Should we keep this memory chation lying around? */
		if (!keep.count(it->first)) {
			BUG_ON(!it->second.empty());
			it = sends.erase(it);
		} else {
			++it;
		}
	}
}

bool SOCalculator::chContains(Channel ch, Event e) const
{
	BUG_ON(sends.count(ch) == 0);
	return e == Event::getInitializer() ||
		std::any_of(send_begin(ch), send_end(ch),
			    [&e](Event s){ return s == e; });
}
