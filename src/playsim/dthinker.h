/*
** dthinker.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#ifndef __DTHINKER_H__
#define __DTHINKER_H__

#include <stdlib.h>
#include "dobject.h"
#include "statnums.h"

class AActor;
class player_t;
struct pspdef_s;
struct FState;
class DThinker;
class FSerializer;
struct FLevelLocals;
struct ProfileInfo;

class FThinkerIterator;

enum { MAX_STATNUM = 127 };

// Doubly linked ring list of thinkers
struct FThinkerList
{
	// No destructor. If this list goes away it's the GC's task to clean the orphaned thinkers. Otherwise this may clash with engine shutdown.
	void AddTail(DThinker *thinker);
	//void AddSleeper(DThinker *thinker);
	void AddHead(DThinker *thinker);
	void AddAfter(DThinker *after, DThinker *thinker);

	DThinker *GetHead() const;
	DThinker *GetTail() const;

	inline void AssertSentinel();

	bool IsEmpty() const;
	void DestroyThinkers();
	bool DoDestroyThinkers(bool& destroyed);
	void RemoveTravellers(bool saveGame);
	void OnLoad();
	int CheckSleepingThinkers(int ticsElapsed = 1);			// Check and unsleep thinkers periodically
	int TickThinkers(FThinkerList *dest, int& counter);	// Returns: # of thinkers ticked
	int ProfileThinkers(FThinkerList *dest, int& counter, TMap<FName, ProfileInfo>& profiles);
	void SaveList(FSerializer &arc);

private:
	DThinker *Sentinel = nullptr;

	friend struct FThinkerCollection;
};

struct FThinkerCollection
{
	void DestroyThinkersInList(int statnum)
	{
		Thinkers[statnum].DestroyThinkers();
		FreshThinkers[statnum].DestroyThinkers();
	}

	void RunThinkers(FLevelLocals *Level);	// The level is needed to tick the lights
	void RunClientSideThinkers(FLevelLocals* Level);
	void DestroyAllThinkers(bool fullgc = true);
	void CleanUpTravellers(bool saveGame);
	void SerializeThinkers(FSerializer &arc, bool keepPlayers);
	void MarkRoots();
	void OnLoad();
	DThinker *FirstThinker(int statnum);
	void Link(DThinker *thinker, int statnum);
	void LinkSleeper(DThinker *thinker, int statnum);
	void CheckSleepers();	// Wake up sleeping thinkers whose time is up

	bool IsSleepCycle() const { return inSleepCycle; }
	void AddWaker(DThinker* einstein) { tempWakers.Push(einstein); }

private:
	FThinkerList Thinkers[MAX_STATNUM + 2];
	FThinkerList FreshThinkers[MAX_STATNUM + 1];

	bool inSleepCycle = false;							// Set when running through sleepers.  If in sleep cycle, we put new sleeping thinkers into FreshThinkers and new wakes into the wake list
	TArray<DThinker*> tempWakers;

	friend class FThinkerIterator;
};

extern bool bTravelling;

class DThinker : public DObject
{
	DECLARE_CLASS (DThinker, DObject)
public:
	static const int DEFAULT_STAT = STAT_DEFAULT;
	void OnDestroy () override;
	virtual ~DThinker ();
	virtual void Tick ();
	void CallTick();
	virtual void PostBeginPlay ();	// Called just before the first tick
	virtual void CallPostBeginPlay(); // different in actor.
	virtual void PostSerialize();
	void CallPostSerialize();

	virtual bool ShouldWake();		// Should wake from sleep
	virtual void Wake();
	virtual void Sleep(int tics = 10);
	virtual void SleepIndefinite();
	virtual void CallSleep(int tics = 10);
	virtual bool CallShouldWake();
	virtual void CallWake();

	void Serialize(FSerializer &arc) override;
	size_t PropagateMark();

	void ChangeStatNum (int statnum);
	inline int GetStatNum() const { return _statNum; }
	// This is temporary and should only be used with the rollback functionality.
	inline void RollbackStatNum(int statNum) { _statNum = statNum; }

private:
	void Remove();

	friend struct FThinkerList;
	friend struct FThinkerCollection;
	friend class FThinkerIterator;
	friend class DObject;
	friend class FDoomSerializer;

	int8_t _statNum = -1;
	DThinker *NextThinker = nullptr, *PrevThinker = nullptr;

	// Sleep info
	int sleepInterval = 0;	// How many tics to sleep before checking for wake
	int sleepTimer = 0;		// Timer data

public:
	FLevelLocals *Level;

	friend struct FLevelLocals;	// Needs access to FreshThinkers until the thinker storage gets refactored.
};

class FThinkerIterator
{
protected:
	const PClass *m_ParentType;
private:
	FLevelLocals *Level;
	FThinkerCollection* m_ThinkerPool;
	DThinker *m_CurrThinker;
	uint8_t m_Stat;
	bool m_SearchStats;
	bool m_SearchingFresh;

public:
	FThinkerIterator (FLevelLocals *Level, const PClass *type, int statnum=MAX_STATNUM+1, bool clientside = false);
	FThinkerIterator (FLevelLocals *Level, const PClass *type, int statnum, DThinker *prev, bool clientside = false);
	DThinker *Next (bool exact = false);
	void Reinit ();
};

template <class T> class TThinkerIterator : public FThinkerIterator
{
public:
	TThinkerIterator (FLevelLocals *Level, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator (Level, RUNTIME_CLASS(T), statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, int statnum, DThinker *prev, bool clientside = false) : FThinkerIterator (Level, RUNTIME_CLASS(T), statnum, prev, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, const PClass *subclass, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator(Level, subclass, statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, FName subclass, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator(Level, PClass::FindClass(subclass), statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, FName subclass, int statnum, DThinker *prev, bool clientside = false) : FThinkerIterator(Level, PClass::FindClass(subclass), statnum, prev, clientside)
	{
	}
	T *Next (bool exact = false)
	{
		return static_cast<T *>(FThinkerIterator::Next (exact));
	}
};


#endif //__DTHINKER_H__
