/*
** hw_spritecensus.cpp
**
** [Karto] Test-only census of the main view's sprites (sprite_census) and the world's model draws
** (model_census), to judge what a per-tic list or an instanced path could take. Off by default.
**
**   sprite_census on | off | print
**
**   model_census on | off | print
*/

#include <algorithm>
#include <typeinfo>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

#include "c_dispatch.h"
#include "printf.h"
#include "actor.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_spriterecord.h"
#include "hw_models.h"
#include "g_levellocals.h"

int WorldModeFrameTic(FLevelLocals* level);

namespace
{
	enum ECensusKind { CK_Record, CK_Face, CK_Model, CK_OtherType, CK_Particle, CK_Split, CK_Count };
	const char* const KindNames[CK_Count] = { "record", "actor_face", "actor_model", "actor_othertype", "particle", "split" };
	const char* const ListNames[GLDL_TYPES] = { "plainwalls", "plainflats", "maskedwalls", "maskedflats", "maskedwallsofs",
		"models", "translucent", "translucentborder", "solidsprites", "addsprites", "alphasprites", "particles",
		"solidparticles", "addparticles" };

	bool Armed = false;
	uint64_t Frames = 0, BaseFrames = 0, ChurnFrames = 0;
	uint64_t Kinds[CK_Count], Lists[GLDL_TYPES];
	uint64_t BaseSizeSum = 0, NewSum = 0, GoneSum = 0, NewMax = 0;
	std::unordered_set<const void*> Base, Seen;
	enum ETransKind { TK_Alpha, TK_Additive, TK_Fuzz, TK_Other, TK_Count };
	const char* const TransNames[TK_Count] = { "alpha", "additive", "fuzz", "other" };
	uint64_t TransKinds[TK_Count], TransBucketable = 0, TransSprites = 0;
	std::unordered_map<std::string, uint64_t> TransClasses, BucketClasses;
	std::unordered_map<std::string, uint64_t> FullClasses;
	uint64_t FullMissile = 0, FullZTrans = 0, FullOther = 0;
	int BaseTic = -1;
	FLevelLocals* BaseLevel = nullptr;
}

void SpriteFrameCensus(HWDrawInfo* di)
{
	if (!Armed) return;
	++Frames;
	Seen.clear();
	for (int list = 0; list < GLDL_TYPES; ++list)
	{
		const auto& sprites = di->drawlists[list].sprites;
		Lists[list] += sprites.Size();
		for (const HWSprite* s : sprites)
		{
			const void* key;
			if (s->particle) { ++Kinds[CK_Particle]; key = s->particle; }
			else if (s->record) { ++Kinds[CK_Record]; key = s->record->Live; }
			else if (s->actor)
			{
				key = s->actor;
				if (s->modelframe) ++Kinds[CK_Model];
				else if ((s->actor->renderflags & RF_SPRITETYPEMASK) != RF_FACESPRITE) ++Kinds[CK_OtherType];
				else ++Kinds[CK_Face];
				if (!s->modelframe && (s->actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
				{
					++FullClasses[s->actor->GetClass()->TypeName.GetChars()];
					if (s->actor->flags & MF_MISSILE) ++FullMissile;
					else if (s->actor->renderflags & RF_ZDOOMTRANS) ++FullZTrans;
					else ++FullOther;
				}
			}
			else continue;
			if (s->lightlist) ++Kinds[CK_Split];
			Seen.insert(key);
			if ((list == GLDL_ALPHASPRITES || list == GLDL_ADDSPRITES || list == GLDL_SOLIDSPRITES) && !s->particle)
			{
				const PClassActor* cls = s->record ? s->record->Class : s->actor ? s->actor->GetClass() : nullptr;
				char key[160];
				snprintf(key, sizeof(key), "%s %s style=%08x trans=%.2f textrans=%d bright=%d hw=%d rec=%d split=%d model=%d", list == GLDL_ALPHASPRITES ? "alpha" : list == GLDL_ADDSPRITES ? "add" : "solid",
					cls ? cls->TypeName.GetChars() : "-", s->RenderStyle.AsDWORD, s->trans, s->texture ? int(s->texture->GetTranslucency()) : -1,
					int(s->fullbright), int(s->hw_styleflags), int(s->record != nullptr), int(s->lightlist != nullptr), int(s->modelframe != nullptr));
				++BucketClasses[key];
			}
			if (list == GLDL_TRANSLUCENT)
			{
				const auto& style = s->RenderStyle;
				int kind = TK_Other;
				if (style.BlendOp == STYLEOP_Fuzz || style.BlendOp == STYLEOP_Shadow || style.BlendOp >= STYLEOP_FuzzOrAdd) kind = TK_Fuzz;
				else if (style.BlendOp == STYLEOP_Add && style.DestAlpha == STYLEALPHA_One) kind = TK_Additive;
				else if (style.BlendOp == STYLEOP_Add && style.DestAlpha == STYLEALPHA_InvSrc) kind = TK_Alpha;
				++TransKinds[kind];
				++TransSprites;
				const bool face = s->record || (s->actor && (s->actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE);
				TransBucketable += kind == TK_Alpha && face && !s->modelframe && !s->lightlist && s->topclip == LARGE_VALUE &&
					s->bottomclip == -LARGE_VALUE;
				const PClassActor* cls = s->record ? s->record->Class : s->actor ? s->actor->GetClass() : nullptr;
				++TransClasses[cls ? cls->TypeName.GetChars() : "particle"];
			}
		}
	}
	const int tic = WorldModeFrameTic(di->Level);
	if (tic != BaseTic || di->Level != BaseLevel)
	{
		BaseTic = tic;
		BaseLevel = di->Level;
		Base = Seen;
		++BaseFrames;
		BaseSizeSum += Base.size();
		return;
	}
	uint64_t added = 0, gone = 0;
	for (const void* key : Seen) added += Base.count(key) == 0;
	for (const void* key : Base) gone += Seen.count(key) == 0;
	++ChurnFrames;
	NewSum += added;
	GoneSum += gone;
	NewMax = std::max(NewMax, added);
}

CCMD(sprite_census)
{
	if (argv.argc() == 2 && stricmp(argv[1], "on") == 0)
	{
		Frames = BaseFrames = ChurnFrames = BaseSizeSum = NewSum = GoneSum = NewMax = 0;
		memset(Kinds, 0, sizeof(Kinds));
		memset(Lists, 0, sizeof(Lists));
		memset(TransKinds, 0, sizeof(TransKinds));
		TransBucketable = TransSprites = 0;
		TransClasses.clear();
		BucketClasses.clear();
		FullClasses.clear();
		FullMissile = FullZTrans = FullOther = 0;
		Base.clear();
		BaseTic = -1;
		BaseLevel = nullptr;
		Armed = true;
		Printf("SPRITECENSUS armed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "off") == 0)
	{
		Armed = false;
		Printf("SPRITECENSUS disarmed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "print") == 0)
	{
		const double f = Frames ? double(Frames) : 1.0;
		Printf("SPRITECENSUS frames=%llu base_frames=%llu", (unsigned long long)Frames, (unsigned long long)BaseFrames);
		for (int k = 0; k < CK_Count; ++k) Printf(" %s=%.1f", KindNames[k], Kinds[k] / f);
		Printf("\nSPRITECENSUSLISTS");
		for (int l = 0; l < GLDL_TYPES; ++l) if (Lists[l]) Printf(" %s=%.1f", ListNames[l], Lists[l] / f);
		const double c = ChurnFrames ? double(ChurnFrames) : 1.0;
		Printf("\nSPRITECENSUSCHURN base_size=%.1f new_per_frame=%.2f gone_per_frame=%.2f new_max=%llu churn_frames=%llu\n",
			BaseFrames ? double(BaseSizeSum) / BaseFrames : 0.0, NewSum / c, GoneSum / c, (unsigned long long)NewMax,
			(unsigned long long)ChurnFrames);
		Printf("SPRITECENSUSTRANS sprites=%.1f bucketable=%.1f", TransSprites / f, TransBucketable / f);
		for (int k = 0; k < TK_Count; ++k) Printf(" %s=%.1f", TransNames[k], TransKinds[k] / f);
		Printf("\n");
		std::vector<std::pair<uint64_t, std::string>> classes;
		for (auto& c : TransClasses) classes.push_back({ c.second, c.first });
		std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for (size_t i = 0; i < classes.size() && i < 15; ++i)
			Printf("SPRITECENSUSTRANSCLASS %s=%.1f\n", classes[i].second.c_str(), classes[i].first / f);
		classes.clear();
		for (auto& c : BucketClasses) classes.push_back({ c.second, c.first });
		std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for (size_t i = 0; i < classes.size() && i < 25; ++i)
			Printf("SPRITECENSUSBUCKET %s=%.1f\n", classes[i].second.c_str(), classes[i].first / f);
		Printf("SPRITECENSUSFULL missile=%.1f zdoomtrans=%.1f other=%.1f\n", FullMissile / f, FullZTrans / f, FullOther / f);
		classes.clear();
		for (auto& c : FullClasses) classes.push_back({ c.second, c.first });
		std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for (size_t i = 0; i < classes.size() && i < 15; ++i)
			Printf("SPRITECENSUSFULLCLASS %s=%.1f\n", classes[i].second.c_str(), classes[i].first / f);
	}
	else Printf("Usage: sprite_census on|off|print\n");
}

bool ModelCensusArmed = false;

namespace
{
	enum EModelBlock { MB_Decoupled, MB_Bones, MB_Clipping, MB_Probe, MB_Blended, MB_Count };
	const char* const BlockNames[MB_Count] = { "decoupled", "bones", "clipping", "lightprobe", "alphablend" };
	enum EModelPass { MP_Opaque, MP_Additive, MP_Blended, MP_Count };
	const char* const PassNames[MP_Count] = { "opaque", "additive", "blended" };

	uint64_t MFrames = 0, MActors = 0, MDraws = 0, MIndices = 0, MMergeable = 0, MKeys = 0, MEligible = 0;
	uint64_t MBlocks[MB_Count], MPasses[MP_Count];
	std::unordered_map<std::string, uint64_t> MClasses, MFormats;
	std::unordered_set<std::string> FrameKeys;

	struct
	{
		bool Open = false, Blocked = false, Bones = false;
		const PClass* Class = nullptr;
		uint32_t Style = 0;
	} Current;

	struct
	{
		bool Valid = false, Indexed = false;
		FModelDrawState Draw;
		size_t End = 0;
	} Previous;

	void CloseModelActor()
	{
		if (!Current.Open) return;
		Current.Open = false;
		if (Current.Bones) ++MBlocks[MB_Bones];
		if (!Current.Blocked && !Current.Bones) ++MEligible;
	}

	bool SameDrawState(const FModelDrawState& a, const FModelDrawState& b)
	{
		return a.Model == b.Model && a.Buffer == b.Buffer && a.Skin == b.Skin && a.Translation == b.Translation &&
			a.Frame1 == b.Frame1 && a.Frame2 == b.Frame2 && a.Bone == b.Bone;
	}
}

void ModelCensusActor(const HWSprite* sprite, HWDrawInfo* di, bool translucent, bool clipping)
{
	CloseModelActor();
	Current.Open = true;
	Current.Bones = false;
	Current.Class = sprite->actor ? sprite->actor->GetClass() : nullptr;
	Current.Style = sprite->RenderStyle.AsDWORD;
	Previous.Valid = false;
	++MActors;

	const auto& style = sprite->RenderStyle;
	const int pass = !translucent ? MP_Opaque : style.BlendOp == STYLEOP_Add && style.DestAlpha == STYLEALPHA_One ? MP_Additive : MP_Blended;
	++MPasses[pass];
	const bool blocks[MB_Count] = {
		sprite->actor && (sprite->actor->flags9 & MF9_DECOUPLEDANIMATIONS),
		false,
		clipping,
		sprite->actor && di->Level->LightProbes.Size() > 0,
		pass == MP_Blended,
	};
	Current.Blocked = false;
	for (int b = 0; b < MB_Count; ++b)
	{
		MBlocks[b] += blocks[b];
		Current.Blocked |= blocks[b];
	}
}

void ModelCensusDraw(const FModelDrawState& draw, bool indexed, size_t first, int count)
{
	if (draw.Hud || !Current.Open) return;
	++MDraws;
	MIndices += count;
	if (draw.Bone >= 0) Current.Bones = true;
	if (Previous.Valid && Previous.Indexed == indexed && first == Previous.End && SameDrawState(draw, Previous.Draw)) ++MMergeable;
	Previous.Valid = true;
	Previous.Indexed = indexed;
	Previous.Draw = draw;
	Previous.End = first + count;

	char key[192];
	snprintf(key, sizeof(key), "%p %p %u %u %d %p %d %08x %d %d %d %zu %d", (const void*)draw.Model, draw.Buffer, draw.Frame1,
		draw.Frame2, int(draw.Bone >= 0), (const void*)draw.Skin, draw.Translation, Current.Style, int(draw.Culled),
		int(draw.Mirrored), int(indexed), first, count);
	FrameKeys.insert(key);
	++MFormats[draw.Model ? typeid(*draw.Model).name() : "-"];
	++MClasses[Current.Class ? Current.Class->TypeName.GetChars() : "-"];
}

void ModelFrameCensus()
{
	if (!ModelCensusArmed) return;
	CloseModelActor();
	++MFrames;
	MKeys += FrameKeys.size();
	FrameKeys.clear();
}

CCMD(model_census)
{
	if (argv.argc() == 2 && stricmp(argv[1], "on") == 0)
	{
		MFrames = MActors = MDraws = MIndices = MMergeable = MKeys = MEligible = 0;
		memset(MBlocks, 0, sizeof(MBlocks));
		memset(MPasses, 0, sizeof(MPasses));
		MClasses.clear();
		MFormats.clear();
		FrameKeys.clear();
		Current.Open = false;
		Previous.Valid = false;
		ModelCensusArmed = true;
		Printf("MODELCENSUS armed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "off") == 0)
	{
		ModelCensusArmed = false;
		Printf("MODELCENSUS disarmed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "print") == 0)
	{
		const double f = MFrames ? double(MFrames) : 1.0;
		Printf("MODELCENSUS frames=%llu actors=%.1f draws=%.1f triangles=%.1f mergeable=%.1f keys=%.1f eligible=%.1f\n",
			(unsigned long long)MFrames, MActors / f, MDraws / f, MIndices / 3 / f, MMergeable / f, MKeys / f, MEligible / f);
		Printf("MODELCENSUSPASS");
		for (int p = 0; p < MP_Count; ++p) Printf(" %s=%.1f", PassNames[p], MPasses[p] / f);
		Printf("\nMODELCENSUSBLOCK");
		for (int b = 0; b < MB_Count; ++b) Printf(" %s=%.1f", BlockNames[b], MBlocks[b] / f);
		Printf("\n");
		for (auto& format : MFormats) Printf("MODELCENSUSFORMAT %s=%.1f\n", format.first.c_str(), format.second / f);
		std::vector<std::pair<uint64_t, std::string>> classes;
		for (auto& c : MClasses) classes.push_back({ c.second, c.first });
		std::sort(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.first > b.first; });
		for (size_t i = 0; i < classes.size() && i < 15; ++i)
			Printf("MODELCENSUSCLASS %s=%.1f\n", classes[i].second.c_str(), classes[i].first / f);
	}
	else Printf("Usage: model_census on|off|print\n");
}
