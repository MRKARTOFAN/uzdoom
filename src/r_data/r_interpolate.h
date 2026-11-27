/*
** r_interpolate.h
**
** Movement interpolation
**
**---------------------------------------------------------------------------
**
** Copyright 2008-2016 Christoph Oelckers
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

#ifndef R_INTERPOLATE_H
#define R_INTERPOLATE_H

#include "dobject.h"

struct FLevelLocals;
//==========================================================================
//
//
//
//==========================================================================

class DInterpolation : public DObject
{
	friend struct FInterpolator;

	DECLARE_ABSTRACT_CLASS(DInterpolation, DObject)
	HAS_OBJECT_POINTERS

	TObjPtr<DInterpolation*> Next;
	TObjPtr<DInterpolation*> Prev;
	// [Karto] Skipped by this frame's DoInterpolations, so RestoreInterpolations must not write its stale backup.
	bool Skipped = false;

protected:
	FLevelLocals *Level;
	int refcount = 0;

	DInterpolation(FLevelLocals *l = nullptr) : Level(l) {}

public:
	int AddRef();
	int DelRef(bool force = false);

	virtual void UnlinkFromMap();
	virtual void UpdateInterpolation() = 0;
	virtual void Restore() = 0;
	virtual void Interpolate(double smoothratio) = 0;
	virtual bool Retired() { return false; }

	virtual void Serialize(FSerializer &arc);
};

//==========================================================================
//
//
//
//==========================================================================

enum EInterpolationSkip
{
	ISKIP_Planes = 1,
	ISKIP_FlatScroll = 2,
	ISKIP_WallScroll = 4,
	ISKIP_Polyobj = 8,
	ISKIP_Other = 16,
	ISKIP_All = ISKIP_Planes | ISKIP_FlatScroll | ISKIP_WallScroll | ISKIP_Polyobj | ISKIP_Other,
};

struct FInterpolator
{
	TObjPtr<DInterpolation*> Head = MakeObjPtr<DInterpolation*>(nullptr);
	bool didInterp = false;
	int count = 0;
	double frac = 1.;

	int CountInterpolations ();

public:
	void UpdateInterpolations();
	void AddInterpolation(DInterpolation *);
	void RemoveInterpolation(DInterpolation *);
	void DoInterpolations(double smoothratio, int skip = 0);
	void RetireSkipped(int skip);
	void RestoreInterpolations();
	void ClearInterpolations();
};


#endif
