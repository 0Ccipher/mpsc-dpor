/*
 * Omkar Tuppe
 */

#ifndef __MPSC_DRIVER_HPP__
#define __MPSC_DRIVER_HPP__

#include "GenMCDriver.hpp"

class MPSCDriver : public GenMCDriver {

public:
	MPSCDriver(std::shared_ptr<const Config> conf, std::unique_ptr<llvm::Module> mod,
		   std::unique_ptr<ModuleInfo> MI);

	void updateLabelViews(EventLabel *lab, const EventDeps *deps) override;
	Event findDataRaceForMemAccess(const MemAccessLabel *mLab) override;
	void changeRf(Event read, Event store) override;
	void changeRf(Channel ch , Event receive, Event send) override;
	void updateStart(Event create, Event start) override;
	bool updateJoin(Event join, Event childLast) override;
	void initConsCalculation() override;

private:
	View calcBasicHbView(Event e) const;
	View calcBasicPorfView(Event e) const;
	void calcWriteMsgView(WriteLabel *lab);
	void calcSendMsgView(SendLabel *lab);
	void calcRMWWriteMsgView(WriteLabel *lab);

	void calcBasicViews(EventLabel *lab);
	void calcReadViews(ReadLabel *lab);
	void calcReceiveViews(ReceiveLabel *lab);
	void calcWriteViews(WriteLabel *lab);
	void calcSendViews(SendLabel *lab);
	void calcFenceViews(FenceLabel *lab);
	void calcStartViews(ThreadStartLabel *lab);
	void calcJoinViews(ThreadJoinLabel *lab);
	void calcFenceRelRfPoBefore(Event last, View &v);

	/* Returns true if aLab and bLab are in an RC11 data race*/
	bool areInDataRace(const MemAccessLabel *aLab, const MemAccessLabel *bLab);

	/* Returns an event that is racy with rLab, or INIT if none is found */
	Event findRaceForNewLoad(const ReadLabel *rLab);

	/* Returns an event that is racy with wLab, or INIT if none is found */
	Event findRaceForNewStore(const WriteLabel *wLab);
};

#endif /* __MPSC_DRIVER_HPP__ */
