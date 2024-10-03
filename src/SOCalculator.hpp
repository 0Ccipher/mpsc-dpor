/*
 * Omkar Tuppe
 */

#ifndef __SO_CALCULATOR_HPP__
#define __SO_CALCULATOR_HPP__

#include "Calculator.hpp"
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#include <unordered_map>

/*******************************************************************************
 **                        SOCalculator Class
 ******************************************************************************/

/*
 * An implementation of the SOCalculator API, that tracks SendOrder
 * by recording the channel send order. Should be used along with the "mo"(for writes)
 * version of the driver.
 */
class SOCalculator : public Calculator {

public:
	typedef int Channel;
	using SendList = std::vector<Event>;
	using ChMap = std::unordered_map<Channel, SendList>;

	using iterator = ChMap::iterator;
	using const_iterator = ChMap::const_iterator;

	using send_iterator = SendList::iterator;
	using const_send_iterator = SendList::const_iterator;

	using reverse_send_iterator = SendList::reverse_iterator;
	using const_reverse_send_iterator = SendList::const_reverse_iterator;

	/* Constructor */
	SOCalculator(ExecutionGraph &g)
		: Calculator(g) {}
	
	virtual ~SOCalculator()  = default;

protected:
	const SendList &getSendsToCh(Channel ch) const { return sends.at(ch); }

	/* Whether the model which we are operating under supports out-of-order
	 * execution. This enables some extra optimizations, in certain cases. */
	bool outOfOrder;

	/* Maps channelid -> send list */
	ChMap sends;

public:

	iterator begin() { return sends.begin(); }
	const_iterator begin() const { return sends.begin(); };
	iterator end() { return sends.end(); }
	const_iterator end() const { return sends.end(); }

	send_iterator send_begin(Channel ch) { return sends[ch].begin(); }
	const_send_iterator send_begin(Channel ch) const { return sends.at(ch).begin(); };
	send_iterator send_end(Channel ch) { return sends[ch].end(); }
	const_send_iterator send_end(Channel ch) const { return sends.at(ch).end(); }

	reverse_send_iterator send_rbegin(Channel ch) { return sends[ch].rbegin(); }
	const_reverse_send_iterator send_rbegin(Channel ch) const { return sends.at(ch).rbegin(); };
	reverse_send_iterator send_rend(Channel ch) { return sends[ch].rend(); }
	const_reverse_send_iterator send_rend(Channel ch) const { return sends.at(ch).rend(); }



	SOCalculator::const_send_iterator
	succ_begin(Channel ch, Event send) const;
	SOCalculator::const_send_iterator
	succ_end(Channel ch, Event send) const;

	SOCalculator::const_send_iterator
	pred_begin(Channel ch, Event send) const;
	SOCalculator::const_send_iterator
	pred_end(Channel ch, Event send) const;


	bool supportsOutOfOrder() {return false;}

	/* Changes the offset of "send" to "newOffset" */
	void changeSendOffset(Channel ch, Event send, int newOffset);

	/* Whether a channel is tracked (i.e., we are aware of it) */
	bool tracksCh(Channel ch) const { return sends.count(ch); }

	/* Whether a channel is empty */
	bool isChEmpty(Channel ch) const { return send_begin(ch) == send_end(ch); }

	/* Whether a channel has more than one send */
	bool hasMoreThanOneSend(Channel ch) const {
		return !isChEmpty(ch) && ++send_begin(ch) != send_end(ch);
	}

	/* Track coherence at channel ch */
	 void
	trackSendOrderAtCh(Channel ch) ;

	/* Returns the range of all the possible (i.e., not violating
	 * coherence) offsets a send can be inserted */
	 std::pair<int, int>
	getPossiblePlacings(Channel ch, Event send) ;

	/* Adds STORE to ADDR at the offset specified by OFFSET.
	 * (Use -1 to insert it maximally.) */
	 void
	addSendToCh(Channel ch, Event send, int offset) ;

	/* Adds STORE to ATTR and ensures it will be co-after PRED.
	 * (Use INIT to insert it minimally.) */
	 void
	addSendToChAfter(Channel ch, Event send, Event pred) ;

	/* Returns whether STORE is maximal in LOC */
	 bool
	isCoMaximal(Channel ch, Event send) ;

	/* Returns whether STORE is maximal in LOC.
	 * Pre: Cached information for this channel exist. */
	 bool
	isCachedCoMaximal(Channel ch, Event send) ;

	/* Returns all the sends for which if "read" reads-from, coherence
	 * is not violated */
	 std::vector<Event>
	getCoherentSends(Channel ch, Event read) ;

	/* Returns all the reads that "wLab" can revisit without violating
	 * coherence */
	 std::vector<Event>
	getCoherentRevisits(const SendLabel *wLab) ;

	// /* Returns whether the path from RLAB to WLAB is maximal */
	//  bool
	// inMaximalPath(const BackwardRevisit &r) ;


	/* Overrided Calculator methods */

	void initCalc() override;

	Calculator::CalculationResult doCalc() override;

	/* Stops tracking all stores not included in "preds" in the graph */
	void removeAfter(const VectorClock &preds) override;

	std::unique_ptr<Calculator> clone(ExecutionGraph &g) const override {
		return std::make_unique<SOCalculator>(g);
	}

private:
	/* Returns the offset for a particular send */
	int getSendOffset(Channel ch, Event e) const;

	/* Returns the index of the first sends that is _not_ (rf?;hb)-before
	 * the event "read". If no such sendss exist (i.e., all sendss are
	 * concurrent in models that do not support out-of-order execution),
	 * it returns 0. */
	int splitChSOBefore(Channel ch, Event read);

	/* Returns the index of the first sends that is hb-after "read",
	 * or the next index of the first sends that is read by a read that
	 * is hb-after "read". Returns 0 if the latter condition holds for
	 * the initializer event, and the number of the sendss in "ch"
	 * if the conditions do not hold for any sends in that channel. */
	int splitChSOAfterHb(Channel ch, const Event read);

	/* Similar to splitChSOAfterHb(), but used for calculating possible SO
	 * placings. This means it does not take into account reads-from the
	 * initializer, and also returns the index (as opposed to index+1) of
	 * the first sends that is hb-after "s" or is read by a read that is
	 * hb-after "s" */
	int splitChSOAfter(Channel ch, const Event s);

	/* Returns the events that are mo;rf?-after sLab */
	std::vector<Event> getSOOptRfAfter(const SendLabel *sLab);

	/* Returns the events that are mo^-1;rf?-after sLab */
	std::vector<Event> getSOInvOptRfAfter(const SendLabel *sLab);

	// bool coherenceSuccRemainInGraph(const BackwardRevisit &r);

	bool wasAddedMaximally(const EventLabel *lab);

	/* Returns true if LAB is co-before any event that would be part
	 * of the saved prefix triggered by the revisit R */
	// bool isCoBeforeSavedPrefix(const BackwardRevisit &r, const EventLabel *lab);

	/* Returns true if the channel "ch" contains the event "e" */
	bool chContains(Channel ch, Event e) const;
};

#endif /* __SO_CALCULATOR_HPP__ */
