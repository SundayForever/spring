/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "UnitDrawer.h"
#include "UnitDrawerState.hpp"

#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GameHelper.h"
#include "Game/GameSetup.h"
#include "Game/GlobalUnsynced.h"
#include "Game/Players/Player.h"
#include "Game/UI/MiniMap.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"

#include "Rendering/Env/ISky.h"
#include "Rendering/Env/IWater.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/FarTextureHandler.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/RenderDataBuffer.hpp"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/Env/IGroundDecalDrawer.h"
#include "Rendering/IconHandler.h"
#include "Rendering/LuaObjectDrawer.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Shaders/Shader.h"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Rendering/Models/ModelRenderContainer.h"

#include "Sim/Features/Feature.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Projectiles/ExplosionGenerator.h"
#include "Sim/Units/BuildInfo.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/Unit.h"

#include "System/Config/ConfigHandler.h"
#include "System/ContainerUtil.h"
#include "System/EventHandler.h"
#include "System/myMath.h"
#include "System/SafeUtil.h"

CUnitDrawer* unitDrawer;

CONFIG(int, UnitLodDist).defaultValue(1000).headlessValue(0);
CONFIG(int, UnitIconDist).defaultValue(200).headlessValue(0);
CONFIG(float, UnitTransparency).defaultValue(0.7f);

CONFIG(int, MaxDynamicModelLights)
	.defaultValue(1)
	.minimumValue(0);




static const void BindOpaqueTex(const CS3OTextureHandler::S3OTexMat* textureMat) {
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex2);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex1);
}

static const void BindOpaqueTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, texturehandler3DO->GetAtlasTex2ID());
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texturehandler3DO->GetAtlasTex1ID());
}
static const void BindOpaqueTexDummy(const CS3OTextureHandler::S3OTexMat*) {}

static const void BindShadowTex(const CS3OTextureHandler::S3OTexMat* textureMat) {
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, textureMat->tex2);
}

static const void KillShadowTex(const CS3OTextureHandler::S3OTexMat*) {
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
}


static const void BindShadowTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texturehandler3DO->GetAtlasTex2ID());
}

static const void KillShadowTexAtlas(const CS3OTextureHandler::S3OTexMat*) {
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
}



static const void PushRenderState3DO() {
	BindOpaqueTexAtlas(nullptr);

	glPushAttrib(GL_POLYGON_BIT);
	glDisable(GL_CULL_FACE);
}

static const void PushRenderStateS3O() {
	glPrimitiveRestartIndex(-1U);
}

static const void PushRenderStateASS() { /* no-op */ }

static const void PopRenderState3DO() { glPopAttrib(); }
static const void PopRenderStateS3O() {    /* no-op */ }
static const void PopRenderStateASS() {    /* no-op */ }


static const void SetTeamColorDummy(const IUnitDrawerState* state, int team, const float2 alpha) {}
static const void SetTeamColorValid(const IUnitDrawerState* state, int team, const float2 alpha) { state->SetTeamColor(team, alpha); }



// typedef std::function<void(int)>                                  BindTexFunc;
// typedef std::function<void(const CS3OTextureHandler::S3OTexMat*)> BindTexFunc;
// typedef std::function<void()>                                     KillTexFunc;
typedef const void (*BindTexFunc)(const CS3OTextureHandler::S3OTexMat*);
typedef const void (*BindTexFunc)(const CS3OTextureHandler::S3OTexMat*);
typedef const void (*KillTexFunc)(const CS3OTextureHandler::S3OTexMat*);

typedef const void (*PushRenderStateFunc)();
typedef const void (*PopRenderStateFunc)();

typedef const void (*SetTeamColorFunc)(const IUnitDrawerState*, int team, const float2 alpha);

static const BindTexFunc opaqueTexBindFuncs[MODELTYPE_OTHER] = {
	BindOpaqueTexDummy, // 3DO (no-op, done by PushRenderState3DO)
	BindOpaqueTex,      // S3O
	BindOpaqueTex,      // ASS
};

static const BindTexFunc shadowTexBindFuncs[MODELTYPE_OTHER] = {
	BindShadowTexAtlas, // 3DO
	BindShadowTex,      // S3O
	BindShadowTex,      // ASS
};

static const BindTexFunc* bindModelTexFuncs[] = {
	&opaqueTexBindFuncs[0], // opaque+alpha
	&shadowTexBindFuncs[0], // shadow
};

static const KillTexFunc shadowTexKillFuncs[MODELTYPE_OTHER] = {
	KillShadowTexAtlas, // 3DO
	KillShadowTex,      // S3O
	KillShadowTex,      // ASS
};


static const PushRenderStateFunc renderStatePushFuncs[MODELTYPE_OTHER] = {
	PushRenderState3DO,
	PushRenderStateS3O,
	PushRenderStateASS,
};

static const PopRenderStateFunc renderStatePopFuncs[MODELTYPE_OTHER] = {
	PopRenderState3DO,
	PopRenderStateS3O,
	PopRenderStateASS,
};


static const SetTeamColorFunc setTeamColorFuncs[] = {
	SetTeamColorDummy,
	SetTeamColorValid,
};



// low-level (batch and solo)
// note: also called during SP
void CUnitDrawer::BindModelTypeTexture(int mdlType, int texType) {
	const auto texFun = bindModelTexFuncs[shadowHandler->InShadowPass()][mdlType];
	const auto texMat = texturehandlerS3O->GetTexture(texType);

	texFun(texMat);
}

void CUnitDrawer::PushModelRenderState(int mdlType) { renderStatePushFuncs[mdlType](); }
void CUnitDrawer::PopModelRenderState (int mdlType) { renderStatePopFuncs [mdlType](); }

// mid-level (solo only)
void CUnitDrawer::PushModelRenderState(const S3DModel* m) {
	PushModelRenderState(m->type);
	BindModelTypeTexture(m->type, m->textureType);
}
void CUnitDrawer::PopModelRenderState(const S3DModel* m) {
	PopModelRenderState(m->type);
}

// high-level (solo only)
void CUnitDrawer::PushModelRenderState(const CSolidObject* o) { PushModelRenderState(o->model); }
void CUnitDrawer::PopModelRenderState(const CSolidObject* o) { PopModelRenderState(o->model); }




CUnitDrawer::CUnitDrawer(): CEventClient("[CUnitDrawer]", 271828, false)
{
	eventHandler.AddClient(this);

	LuaObjectDrawer::ReadLODScales(LUAOBJ_UNIT);
	SetUnitDrawDist((float)configHandler->GetInt("UnitLodDist"));
	SetUnitIconDist((float)configHandler->GetInt("UnitIconDist"));

	alphaValues.x = std::max(0.11f, std::min(1.0f, 1.0f - configHandler->GetFloat("UnitTransparency")));
	alphaValues.y = std::min(1.0f, alphaValues.x + 0.1f);
	alphaValues.z = std::min(1.0f, alphaValues.x + 0.2f);
	alphaValues.w = std::min(1.0f, alphaValues.x + 0.4f);

	// load unit explosion generators and decals
	for (size_t unitDefID = 1; unitDefID < unitDefHandler->unitDefs.size(); unitDefID++) {
		UnitDef& ud = unitDefHandler->unitDefs[unitDefID];

		for (unsigned int n = 0; n < ud.modelCEGTags.size(); n++) {
			ud.SetModelExplosionGeneratorID(n, explGenHandler->LoadGeneratorID(ud.modelCEGTags[n]));
		}
		for (unsigned int n = 0; n < ud.pieceCEGTags.size(); n++) {
			// these can only be custom EG's so prefix is not required game-side
			ud.SetPieceExplosionGeneratorID(n, explGenHandler->LoadGeneratorID(CEG_PREFIX_STRING + ud.pieceCEGTags[n]));
		}
	}


	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		opaqueModelRenderers[modelType] = IModelRenderContainer::GetInstance(modelType);
		alphaModelRenderers[modelType] = IModelRenderContainer::GetInstance(modelType);
	}

	deadGhostBuildings.resize(teamHandler->ActiveAllyTeams());
	liveGhostBuildings.resize(teamHandler->ActiveAllyTeams());

	// LH must be initialized before drawer-state is initialized
	lightHandler.Init(configHandler->GetInt("MaxDynamicModelLights"));

	unitDrawerStates[DRAWER_STATE_NOP] = IUnitDrawerState::GetInstance( true, false);
	unitDrawerStates[DRAWER_STATE_SSP] = IUnitDrawerState::GetInstance(false, false);
	unitDrawerStates[DRAWER_STATE_LUA] = IUnitDrawerState::GetInstance(false,  true);

	drawModelFuncs[0] = &CUnitDrawer::DrawUnitModelBeingBuiltOpaque;
	drawModelFuncs[1] = &CUnitDrawer::DrawUnitModelBeingBuiltShadow;
	drawModelFuncs[2] = &CUnitDrawer::DrawUnitModel;

	// shared with FeatureDrawer!
	geomBuffer = LuaObjectDrawer::GetGeometryBuffer();

	drawForward = true;
	drawDeferred = (geomBuffer->Valid());
	wireFrameMode = false;

	unitDrawerStates[DRAWER_STATE_SSP]->Init(this);
	cubeMapHandler->Init(); // can only fail if FBO's are invalid

	// note: state must be pre-selected before the first drawn frame
	// Sun*Changed can be called first, e.g. if DynamicSun is enabled
	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(false));
}

CUnitDrawer::~CUnitDrawer()
{
	eventHandler.RemoveClient(this);

	unitDrawerStates[DRAWER_STATE_NOP]->Kill(); IUnitDrawerState::FreeInstance(unitDrawerStates[DRAWER_STATE_NOP]);
	unitDrawerStates[DRAWER_STATE_SSP]->Kill(); IUnitDrawerState::FreeInstance(unitDrawerStates[DRAWER_STATE_SSP]);
	unitDrawerStates[DRAWER_STATE_LUA]->Kill(); IUnitDrawerState::FreeInstance(unitDrawerStates[DRAWER_STATE_LUA]);

	cubeMapHandler->Free();

	for (CUnit* u: unsortedUnits) {
		groundDecals->ForceRemoveSolidObject(u);
	}

	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			auto& lgb = liveGhostBuildings[allyTeam][modelType];
			auto& dgb = deadGhostBuildings[allyTeam][modelType];

			for (auto it = dgb.begin(); it != dgb.end(); ++it) {
				GhostSolidObject* gso = *it;

				if (gso->DecRef())
					continue;

				// <ghost> might be the gbOwner of a decal; groundDecals is deleted after us
				groundDecals->GhostDestroyed(gso);
				spring::SafeDelete(*it);
			}

			dgb.clear();
			lgb.clear();
		}
	}
	deadGhostBuildings.clear();
	liveGhostBuildings.clear();


	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		delete opaqueModelRenderers[modelType];
		delete alphaModelRenderers[modelType];
	}

	unsortedUnits.clear();
}



void CUnitDrawer::SetUnitDrawDist(float dist)
{
	unitDrawDist = dist;
	unitDrawDistSqr = dist * dist;
}

void CUnitDrawer::SetUnitIconDist(float dist)
{
	unitIconDist = dist;
	iconLength = 750.0f * unitIconDist * unitIconDist;
}



void CUnitDrawer::Update()
{
	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		UpdateTempDrawUnits(tempOpaqueUnits[modelType]);
		UpdateTempDrawUnits(tempAlphaUnits[modelType]);
	}

	{
		iconUnits.clear();

		for (CUnit* unit: unsortedUnits) {
			UpdateUnitIconState(unit);
			UpdateUnitDrawPos(unit);
		}
	}

	if ((useDistToGroundForIcons = (camHandler->GetCurrentController()).GetUseDistToGroundForIcons())) {
		const float3& camPos = camera->GetPos();
		// use the height at the current camera position
		//const float groundHeight = CGround::GetHeightAboveWater(camPos.x, camPos.z, false);
		// use the middle between the highest and lowest position on the map as average
		const float groundHeight = (readMap->GetCurrMinHeight() + readMap->GetCurrMaxHeight()) * 0.5f;
		const float overGround = camPos.y - groundHeight;

		sqCamDistToGroundForIcons = overGround * overGround;
	}
}




void CUnitDrawer::Draw()
{
	assert((CCamera::GetActiveCamera())->GetCamType() != CCamera::CAMTYPE_SHADOW);

	// first do the deferred pass; conditional because
	// most of the water renderers use their own FBO's
	if (drawDeferred && !water->DrawReflectionPass() && !water->DrawRefractionPass())
		LuaObjectDrawer::DrawDeferredPass(LUAOBJ_UNIT);

	// now do the regular forward pass
	if (drawForward)
		DrawOpaquePass(false);

	farTextureHandler->Draw();

	glDisable(GL_ALPHA_TEST);
}

void CUnitDrawer::DrawOpaquePass(bool deferredPass)
{
	SetupOpaqueDrawing(deferredPass);

	// cubemap reflections do not include units
	const bool inWaterReflPass = water->DrawReflectionPass();
	const bool inWaterRefrPass = water->DrawRefractionPass();

	if (inWaterReflPass)
		unitDrawerStates[DRAWER_STATE_SEL]->SetWaterClipPlane(DrawPass::WaterReflection);
	if (inWaterRefrPass)
		unitDrawerStates[DRAWER_STATE_SEL]->SetWaterClipPlane(DrawPass::WaterRefraction);

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		PushModelRenderState(modelType);
		DrawOpaqueUnits(modelType, inWaterReflPass, inWaterRefrPass);
		DrawOpaqueAIUnits(modelType);
		PopModelRenderState(modelType);
	}

	ResetOpaqueDrawing(deferredPass);

	// draw all custom'ed units that were bypassed in the loop above
	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawOpaqueMaterialObjects(LUAOBJ_UNIT, deferredPass);
}



void CUnitDrawer::DrawOpaqueUnits(int modelType, bool drawReflection, bool drawRefraction)
{
	const auto& unitBin = opaqueModelRenderers[modelType]->GetUnitBin();

	for (const auto& unitBinPair: unitBin) {
		const auto& unitSet = unitBinPair.second;
		const int textureType = unitBinPair.first;

		BindModelTypeTexture(modelType, textureType);

		for (CUnit* unit: unitSet) {
			DrawOpaqueUnit(unit, drawReflection, drawRefraction);
		}
	}
}

inline void CUnitDrawer::DrawOpaqueUnit(CUnit* unit, bool drawReflection, bool drawRefraction)
{
	if (!CanDrawOpaqueUnit(unit, drawReflection, drawRefraction))
		return;

	if ((unit->pos).SqDistance(camera->GetPos()) > (unit->sqRadius * unitDrawDistSqr)) {
		farTextureHandler->Queue(unit);
		return;
	}

	if (LuaObjectDrawer::AddOpaqueMaterialObject(unit, LUAOBJ_UNIT))
		return;

	// draw the unit with the default (non-Lua) material
	SetTeamColour(unit->team);
	DrawUnitDefTrans(unit, false, false);
}


void CUnitDrawer::DrawOpaqueAIUnits(int modelType)
{
	const std::vector<TempDrawUnit>& tmpOpaqueUnits = tempOpaqueUnits[modelType];

	// NOTE: not type-sorted
	for (const TempDrawUnit& unit: tmpOpaqueUnits) {
		if (!camera->InView(unit.pos, 100.0f))
			continue;

		DrawOpaqueAIUnit(unit);
	}
}

void CUnitDrawer::DrawOpaqueAIUnit(const TempDrawUnit& unit)
{
	CMatrix44f mat;
	mat.Translate(unit.pos);
	mat.RotateY(unit.rotation * math::RAD_TO_DEG);

	const IUnitDrawerState* state = GetDrawerState(DRAWER_STATE_SEL);

	const UnitDef* def = unit.unitDef;
	const S3DModel* mdl = def->model;

	assert(mdl != nullptr);

	BindModelTypeTexture(mdl->type, mdl->textureType);
	SetTeamColour(unit.team);
	state->SetMatrices(mat, mdl->GetPieceMatrices());
	mdl->Draw();
}



void CUnitDrawer::DrawUnitIcons()
{
	// draw unit icons and radar blips
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.05f);

	// A2C effectiveness is limited below four samples
	if (globalRendering->msaaLevel >= 4)
		glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);

	for (CUnit* u: iconUnits) {
		const unsigned short closBits = (u->losStatus[gu->myAllyTeam] & (LOS_INLOS                  ));
		const unsigned short plosBits = (u->losStatus[gu->myAllyTeam] & (LOS_PREVLOS | LOS_CONTRADAR));

		DrawIcon(u, !gu->spectatingFullView && closBits == 0 && plosBits != (LOS_PREVLOS | LOS_CONTRADAR));
	}

	glPopAttrib();
}



/******************************************************************************/
/******************************************************************************/

bool CUnitDrawer::CanDrawOpaqueUnit(
	const CUnit* unit,
	bool drawReflection,
	bool drawRefraction
) const {
	if (unit == (drawReflection? nullptr: (gu->GetMyPlayer())->fpsController.GetControllee()))
		return false;
	if (unit->noDraw)
		return false;
	if (unit->IsInVoid())
		return false;
	// unit will be drawn as icon instead
	if (unit->isIcon)
		return false;

	if (!(unit->losStatus[gu->myAllyTeam] & LOS_INLOS) && !gu->spectatingFullView)
		return false;

	// either PLAYER or UWREFL
	const CCamera* cam = CCamera::GetActiveCamera();

	if (drawRefraction && !unit->IsInWater())
		return false;

	if (drawReflection && !ObjectVisibleReflection(unit->drawMidPos, cam->GetPos(), unit->GetDrawRadius()))
		return false;

	return (cam->InView(unit->drawMidPos, unit->GetDrawRadius()));
}

bool CUnitDrawer::CanDrawOpaqueUnitShadow(const CUnit* unit) const
{
	if (unit->noDraw)
		return false;
	if (unit->IsInVoid())
		return false;
	// no shadow if unit is already an icon from player's POV
	if (unit->isIcon)
		return false;
	if (unit->isCloaked)
		return false;

	const CCamera* cam = CCamera::GetActiveCamera();

	const bool unitInLOS = ((unit->losStatus[gu->myAllyTeam] & LOS_INLOS) || gu->spectatingFullView);
	const bool unitInView = cam->InView(unit->drawMidPos, unit->GetDrawRadius());

	return (unitInLOS && unitInView);
}




void CUnitDrawer::DrawOpaqueUnitShadow(CUnit* unit) {
	if (!CanDrawOpaqueUnitShadow(unit))
		return;

	if (LuaObjectDrawer::AddShadowMaterialObject(unit, LUAOBJ_UNIT))
		return;

	DrawUnitDefTrans(unit, false, false);
}


void CUnitDrawer::DrawOpaqueUnitsShadow(int modelType) {
	const auto& unitBin = opaqueModelRenderers[modelType]->GetUnitBin();

	for (const auto& unitBinPair: unitBin) {
		const auto& unitSet = unitBinPair.second;
		const int textureType = unitBinPair.first;

		// only need to bind the atlas once for 3DO's, but KISS
		assert((modelType != MODELTYPE_3DO) || (textureType == 0));
		shadowTexBindFuncs[modelType](texturehandlerS3O->GetTexture(textureType));

		for (const auto& unitSetP: unitSet) {
			DrawOpaqueUnitShadow(unitSetP);
		}

		shadowTexKillFuncs[modelType](nullptr);
	}
}

void CUnitDrawer::DrawShadowPass()
{
	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);

	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);

	Shader::IProgramObject* po = shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);
	po->Enable();
	// TODO: shared by {AdvTree,SMFGround,Unit,Feature,Projectile,LuaObject}Drawer, move to ShadowHandler
	po->SetUniformMatrix4fv(1, false, shadowHandler->GetShadowViewMatrix());
	po->SetUniformMatrix4fv(2, false, shadowHandler->GetShadowProjMatrix());

	{
		assert((CCamera::GetActiveCamera())->GetCamType() == CCamera::CAMTYPE_SHADOW);

		// 3DO's have clockwise-wound faces and
		// (usually) holes, so disable backface
		// culling for them
		glDisable(GL_CULL_FACE);
		DrawOpaqueUnitsShadow(MODELTYPE_3DO);
		glEnable(GL_CULL_FACE);

		for (int modelType = MODELTYPE_S3O; modelType < MODELTYPE_OTHER; modelType++) {
			// note: just use DrawOpaqueUnits()? would
			// save texture switches needed anyway for
			// alpha-masking
			DrawOpaqueUnitsShadow(modelType);
		}
	}

	po->Disable();

	glDisable(GL_ALPHA_TEST);
	glDisable(GL_POLYGON_OFFSET_FILL);

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawShadowMaterialObjects(LUAOBJ_UNIT, false);
}



void CUnitDrawer::DrawIcon(CUnit* unit, bool useDefaultIcon)
{
	// iconUnits should not never contain void-space units, see UpdateUnitIconState
	assert(!unit->IsInVoid());

	// If the icon is to be drawn as a radar blip, we want to get the default icon.
	const icon::CIconData* iconData = nullptr;

	if (useDefaultIcon) {
		iconData = icon::iconHandler->GetDefaultIconData();
	} else {
		iconData = unit->unitDef->iconType.GetIconData();
	}

	// drawMidPos is auto-calculated now; can wobble on its own as pieces move
	float3 pos = (!gu->spectatingFullView) ?
		unit->GetObjDrawErrorPos(gu->myAllyTeam) :
		unit->GetObjDrawMidPos();

	// make sure icon is above ground (needed before we calculate scale below)
	const float h = CGround::GetHeightReal(pos.x, pos.z, false);

	pos.y = std::max(pos.y, h);

	// Calculate the icon size. It scales with:
	//  * The square root of the camera distance.
	//  * The mod defined 'iconSize' (which acts a multiplier).
	//  * The unit radius, depending on whether the mod defined 'radiusadjust' is true or false.
	const float dist = std::min(8000.0f, fastmath::sqrt_builtin(camera->GetPos().SqDistance(pos)));
	const float iconScale = 0.4f * fastmath::sqrt_builtin(dist); // makes far icons bigger
	float scale = iconData->GetSize() * iconScale;

	if (iconData->GetRadiusAdjust() && !useDefaultIcon)
		scale *= (unit->radius / iconData->GetRadiusScale());

	// make sure icon is not partly under ground
	pos.y = std::max(pos.y, h + scale);

	// store the icon size so that we don't have to calculate it again
	unit->iconRadius = scale;

	// Is the unit selected? Then draw it white.
	if (unit->isSelected) {
		glColor3ub(255, 255, 255);
	} else {
		glColor3ubv(teamHandler->Team(unit->team)->color);
	}

	// calculate the vertices
	const float3 dy = camera->GetUp()    * scale;
	const float3 dx = camera->GetRight() * scale;
	const float3 vn = pos - dx;
	const float3 vp = pos + dx;
	const float3 vnn = vn - dy;
	const float3 vpn = vp - dy;
	const float3 vnp = vn + dy;
	const float3 vpp = vp + dy;

	// Draw the icon.
	iconData->Draw(vnn, vpn, vnp, vpp);
}






void CUnitDrawer::SetupAlphaDrawing(bool deferredPass)
{
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_POLYGON_BIT);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE * wireFrameMode + GL_FILL * (1 - wireFrameMode));

	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(true));
	unitDrawerStates[DRAWER_STATE_SEL]->Enable(this, deferredPass && false, true);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.1f);
	glDepthMask(GL_FALSE);
}

void CUnitDrawer::ResetAlphaDrawing(bool deferredPass)
{
	unitDrawerStates[DRAWER_STATE_SEL]->Disable(this, deferredPass && false);

	glPopAttrib();
}



void CUnitDrawer::DrawAlphaPass()
{
	{
		SetupAlphaDrawing(false);
		glDisable(GL_ALPHA_TEST);

		if (water->DrawReflectionPass())
			unitDrawerStates[DRAWER_STATE_SEL]->SetWaterClipPlane(DrawPass::WaterReflection);
		if (water->DrawRefractionPass())
			unitDrawerStates[DRAWER_STATE_SEL]->SetWaterClipPlane(DrawPass::WaterRefraction);

		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			PushModelRenderState(modelType);
			DrawAlphaUnits(modelType);
			DrawAlphaAIUnits(modelType);
			PopModelRenderState(modelType);
		}

		glEnable(GL_ALPHA_TEST);
		ResetAlphaDrawing(false);
	}

	LuaObjectDrawer::SetDrawPassGlobalLODFactor(LUAOBJ_UNIT);
	LuaObjectDrawer::DrawAlphaMaterialObjects(LUAOBJ_UNIT, false);
}

void CUnitDrawer::DrawAlphaUnits(int modelType)
{
	{
		const auto mdlRenderer = alphaModelRenderers[modelType];
		const auto& unitBin = mdlRenderer->GetUnitBin();

		for (const auto& binElem: unitBin) {
			BindModelTypeTexture(modelType, binElem.first);

			for (CUnit* unit: binElem.second) {
				DrawAlphaUnit(unit, modelType, false);
			}
		}
	}

	// living and dead ghosted buildings
	if (!gu->spectatingFullView)
		DrawGhostedBuildings(modelType);
}

inline void CUnitDrawer::DrawAlphaUnit(CUnit* unit, int modelType, bool drawGhostBuildingsPass) {
	if (!camera->InView(unit->drawMidPos, unit->GetDrawRadius()))
		return;

	if (LuaObjectDrawer::AddAlphaMaterialObject(unit, LUAOBJ_UNIT))
		return;

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	if (drawGhostBuildingsPass) {
		const IUnitDrawerState* state = GetDrawerState(DRAWER_STATE_SEL);

		// check for decoy models
		const UnitDef* decoyDef = unit->unitDef->decoyDef;
		const S3DModel* model = nullptr;

		if (decoyDef == nullptr) {
			model = unit->model;
		} else {
			model = decoyDef->LoadModel();
		}

		// FIXME: needs a second pass
		if (model->type != modelType)
			return;

		// ghosted enemy units
		if (losStatus & LOS_CONTRADAR) {
			glColor4f(0.9f, 0.9f, 0.9f, alphaValues.z);
		} else {
			glColor4f(0.6f, 0.6f, 0.6f, alphaValues.y);
		}

		CMatrix44f mat;
		mat.Translate(unit->drawPos);
		mat.RotateY(unit->buildFacing * 90.0f);

		// the units in liveGhostedBuildings[modelType] are not
		// sorted by textureType, but we cannot merge them with
		// alphaModelRenderers[modelType] either since they are
		// not actually cloaked
		BindModelTypeTexture(modelType, model->textureType);

		SetTeamColour(unit->team, float2((losStatus & LOS_CONTRADAR)? alphaValues.z: alphaValues.y, 1.0f));
		state->SetMatrices(mat, model->GetPieceMatrices());
		model->Draw();

		glColor4f(1.0f, 1.0f, 1.0f, alphaValues.x);
		return;
	}

	if (unit->isIcon)
		return;

	if ((losStatus & LOS_INLOS) || gu->spectatingFullView) {
		SetTeamColour(unit->team, float2(alphaValues.x, 1.0f));
		DrawUnitDefTrans(unit, false, false);
	}
}



void CUnitDrawer::DrawAlphaAIUnits(int modelType)
{
	std::vector<TempDrawUnit>& tmpAlphaUnits = tempAlphaUnits[modelType];

	// NOTE: not type-sorted
	for (const TempDrawUnit& unit: tmpAlphaUnits) {
		if (!camera->InView(unit.pos, 100.0f))
			continue;

		DrawAlphaAIUnit(unit);
		DrawAlphaAIUnitBorder(unit);
	}
}

void CUnitDrawer::DrawAlphaAIUnit(const TempDrawUnit& unit)
{
	CMatrix44f mat;
	mat.Translate(unit.pos);
	mat.RotateY(unit.rotation * math::RAD_TO_DEG);

	const IUnitDrawerState* state = GetDrawerState(DRAWER_STATE_SEL);

	const UnitDef* def = unit.unitDef;
	const S3DModel* mdl = def->model;

	assert(mdl != nullptr);

	BindModelTypeTexture(mdl->type, mdl->textureType);
	SetTeamColour(unit.team, float2(alphaValues.x, 1.0f));
	state->SetMatrices(mat, mdl->GetPieceMatrices());
	mdl->Draw();
}

void CUnitDrawer::DrawAlphaAIUnitBorder(const TempDrawUnit& unit)
{
	if (!unit.drawBorder)
		return;

	SetTeamColour(unit.team, float2(alphaValues.w, 1.0f));

	const BuildInfo buildInfo(unit.unitDef, unit.pos, unit.facing);
	const float3 buildPos = CGameHelper::Pos2BuildPos(buildInfo, false);

	const float xsize = buildInfo.GetXSize() * (SQUARE_SIZE >> 1);
	const float zsize = buildInfo.GetZSize() * (SQUARE_SIZE >> 1);

	glColor4f(0.2f, 1, 0.2f, alphaValues.w);
	glDisable(GL_TEXTURE_2D);
	glBegin(GL_LINE_STRIP);
		glVertexf3(buildPos + float3( xsize, 1.0f,  zsize));
		glVertexf3(buildPos + float3(-xsize, 1.0f,  zsize));
		glVertexf3(buildPos + float3(-xsize, 1.0f, -zsize));
		glVertexf3(buildPos + float3( xsize, 1.0f, -zsize));
		glVertexf3(buildPos + float3( xsize, 1.0f,  zsize));
	glEnd();
	glColor4f(1.0f, 1.0f, 1.0f, alphaValues.x);
	glEnable(GL_TEXTURE_2D);
}

void CUnitDrawer::UpdateGhostedBuildings()
{
	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
			auto& dgb = deadGhostBuildings[allyTeam][modelType];

			for (size_t i = 0; i < dgb.size(); /*no-op*/) {
				GhostSolidObject* gso = dgb[i];

				if (!losHandler->InLos(gso->pos, allyTeam)) {
					++i;
					continue;
				}

				// obtained LOS on the ghost of a dead building
				if (!gso->DecRef()) {
					groundDecals->GhostDestroyed(gso);
					spring::SafeDelete(gso);
				}

				dgb[i] = dgb.back();
				dgb.pop_back();
			}
		}
	}
}

void CUnitDrawer::DrawGhostedBuildings(int modelType)
{
	assert((unsigned) gu->myAllyTeam < deadGhostBuildings.size());

	std::vector<GhostSolidObject*>& deadGhostedBuildings = deadGhostBuildings[gu->myAllyTeam][modelType];
	std::vector<CUnit*>& liveGhostedBuildings = liveGhostBuildings[gu->myAllyTeam][modelType];

	const IUnitDrawerState* state = GetDrawerState(DRAWER_STATE_SEL);

	glColor4f(0.6f, 0.6f, 0.6f, alphaValues.y);

	// buildings that died while ghosted
	for (GhostSolidObject* gso: deadGhostedBuildings) {
		const S3DModel* model = gso->model;

		if (!camera->InView(gso->pos, model->GetDrawRadius()))
			continue;

		CMatrix44f mat;
		mat.Translate(gso->pos);
		mat.RotateY(gso->facing * 90.0f);

		BindModelTypeTexture(modelType, gso->model->textureType);
		SetTeamColour(gso->team, float2(alphaValues.y, 1.0f));
		state->SetMatrices(mat, model->GetPieceMatrices());
		model->Draw();

		gso->lastDrawFrame = globalRendering->drawFrame;
	}

	for (CUnit* u: liveGhostedBuildings) {
		DrawAlphaUnit(u, modelType, true);
	}
}






void CUnitDrawer::SetupOpaqueDrawing(bool deferredPass)
{
	glPushAttrib(GL_ENABLE_BIT | GL_POLYGON_BIT);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE * wireFrameMode + GL_FILL * (1 - wireFrameMode));

	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);

	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);

	// pick base shaders (GLSL); not used by custom-material models
	unitDrawerStates[DRAWER_STATE_SEL] = const_cast<IUnitDrawerState*>(GetWantedDrawerState(false));
	unitDrawerStates[DRAWER_STATE_SEL]->Enable(this, deferredPass, false);

	// NOTE:
	//   when deferredPass is true we MUST be able to use the SSP render-state
	//   all calling code (reached from DrawOpaquePass(deferred=true)) should
	//   ensure this is the case
	assert(!deferredPass || true);
	assert(!deferredPass || unitDrawerStates[DRAWER_STATE_SEL]->CanDrawDeferred());
}

void CUnitDrawer::ResetOpaqueDrawing(bool deferredPass)
{
	unitDrawerStates[DRAWER_STATE_SEL]->Disable(this, deferredPass);

	glPopAttrib();
}




void CUnitDrawer::SetTeamColour(int team, const float2 alpha) const
{
	// need this because we can be called by no-team projectiles
	const int b0 = teamHandler->IsValidTeam(team);
	// should be an assert, but projectiles (+FlyingPiece) would trigger it
	const int b1 = !shadowHandler->InShadowPass();

	setTeamColorFuncs[b0 * b1](unitDrawerStates[DRAWER_STATE_SEL], team, alpha);
}


void CUnitDrawer::PushIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass)
{
	// these are not handled by Setup*Drawing but CGame
	// easier to assume they no longer have the correct
	// values at this point
	glPushAttrib(GL_DEPTH_BUFFER_BIT | GL_ENABLE_BIT);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	SetupOpaqueDrawing(deferredPass);
	PushModelRenderState(model);
	SetTeamColour(teamID);
}

void CUnitDrawer::PushIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass)
{
	SetupAlphaDrawing(deferredPass);
	PushModelRenderState(model);
	SetTeamColour(teamID, float2(alphaValues.x, 1.0f));
}


void CUnitDrawer::PopIndividualOpaqueState(const S3DModel* model, int teamID, bool deferredPass)
{
	PopModelRenderState(model);
	ResetOpaqueDrawing(deferredPass);

	glPopAttrib();
}

void CUnitDrawer::PopIndividualAlphaState(const S3DModel* model, int teamID, bool deferredPass)
{
	PopModelRenderState(model);
	ResetAlphaDrawing(deferredPass);
}


void CUnitDrawer::PushIndividualOpaqueState(const CUnit* unit, bool deferredPass)
{
	PushIndividualOpaqueState(unit->model, unit->team, deferredPass);
}

void CUnitDrawer::PopIndividualOpaqueState(const CUnit* unit, bool deferredPass)
{
	PopIndividualOpaqueState(unit->model, unit->team, deferredPass);
}


void CUnitDrawer::DrawIndividualDefTrans(const CUnit* unit, bool noLuaCall)
{
	if (LuaObjectDrawer::DrawSingleObject(unit, LUAOBJ_UNIT /*, noLuaCall*/))
		return;

	// set the full default state
	PushIndividualOpaqueState(unit, false);
	DrawUnitDefTrans(unit, false, noLuaCall);
	PopIndividualOpaqueState(unit, false);
}

void CUnitDrawer::DrawIndividualLuaTrans(const CUnit* unit, bool noLuaCall)
{
	if (LuaObjectDrawer::DrawSingleObjectLuaTrans(unit, LUAOBJ_UNIT /*, noLuaCall*/))
		return;

	PushIndividualOpaqueState(unit, false);
	DrawUnitLuaTrans(unit, false, noLuaCall);
	PopIndividualOpaqueState(unit, false);
}




static void DIDResetPrevProjection(bool toScreen)
{
	if (!toScreen)
		return;

	GL::MatrixMode(GL_PROJECTION);
	GL::PopMatrix();
	GL::PushMatrix();
}

static void DIDResetPrevModelView()
{
	GL::MatrixMode(GL_MODELVIEW);
	GL::PopMatrix();
	GL::PushMatrix();
}

static bool DIDCheckMatrixMode(int wantedMode)
{
	#if 1
	int matrixMode = 0;
	glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
	return (matrixMode == wantedMode);
	#else
	return true;
	#endif
}


// used by LuaOpenGL::Draw{Unit,Feature}Shape
// acts like DrawIndividual but can not apply
// custom materials
void CUnitDrawer::DrawObjectDefOpaque(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen)
{
	// TODO: use the matrix stack
	return;

	const IUnitDrawerState* state = nullptr;
	const S3DModel* model = objectDef->LoadModel();

	if (model == nullptr)
		return;

	if (!rawState) {
		if (!DIDCheckMatrixMode(GL_MODELVIEW))
			return;

		// teamID validity is checked by SetTeamColour
		unitDrawer->PushIndividualOpaqueState(model, teamID, false);

		// NOTE:
		//   unlike DrawIndividual(...) the model transform is
		//   always provided by Lua, not taken from the object
		//   (which does not exist here) so we must restore it
		//   (by undoing the UnitDrawerState MVP setup)
		//
		//   assumes the Lua transform includes a LoadIdentity!
		DIDResetPrevProjection(toScreen);
		DIDResetPrevModelView();

		state = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);
		state->SetMatrices(GL::GetMatrix(), model->GetPieceMatrices());
	}

	model->Draw();

	if (!rawState) {
		unitDrawer->PopIndividualOpaqueState(model, teamID, false);
	}
}

// used for drawing building orders (with translucency)
void CUnitDrawer::DrawObjectDefAlpha(const SolidObjectDef* objectDef, int teamID, bool rawState, bool toScreen)
{
	// TODO: use the matrix stack
	return;

	const IUnitDrawerState* state = nullptr;
	const S3DModel* model = objectDef->LoadModel();

	if (model == nullptr)
		return;

	if (!rawState) {
		if (!DIDCheckMatrixMode(GL_MODELVIEW))
			return;

		unitDrawer->PushIndividualAlphaState(model, teamID, false);

		DIDResetPrevProjection(toScreen);
		DIDResetPrevModelView();

		state = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);
		state->SetMatrices(GL::GetMatrix(), model->GetPieceMatrices());
	}

	model->Draw();

	if (!rawState) {
		unitDrawer->PopIndividualAlphaState(model, teamID, false);
	}
}






typedef const void (*DrawModelBuildStageFunc)(const CUnit*, const IUnitDrawerState* state, const float4&, const float4&, bool);

static const void DrawModelNoopBuildStage(const CUnit*, const IUnitDrawerState* state, const float4&, const float4&, bool)
{
}

static const void DrawModelWireBuildStage(
	const CUnit* unit,
	const IUnitDrawerState* state,
	const float4& upperPlane,
	const float4& lowerPlane,
	bool noLuaCall
) {
	state->SetBuildClipPlanes(upperPlane, lowerPlane);

	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static const void DrawModelFlatBuildStage(
	const CUnit* unit,
	const IUnitDrawerState* state,
	const float4& upperPlane,
	const float4& lowerPlane,
	bool noLuaCall
) {
	state->SetBuildClipPlanes(upperPlane, lowerPlane);

	CUnitDrawer::DrawUnitModel(unit, noLuaCall);
}

static const void DrawModelFillBuildStage(
	const CUnit* unit,
	const IUnitDrawerState* state,
	const float4& upperPlane,
	const float4& lowerPlane,
	bool noLuaCall
) {
	state->SetBuildClipPlanes(upperPlane, lowerPlane);

	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);
		CUnitDrawer::DrawUnitModel(unit, noLuaCall);
	glDisable(GL_POLYGON_OFFSET_FILL);
}

static const DrawModelBuildStageFunc drawModelBuildStageFuncs[] = {
	DrawModelNoopBuildStage,
	DrawModelWireBuildStage,
	DrawModelFlatBuildStage,
	DrawModelFillBuildStage,
};




void CUnitDrawer::DrawUnitModelBeingBuiltShadow(const CUnit* unit, bool noLuaCall)
{
	if (unit->buildProgress <= 0.666f)
		return;

	DrawUnitModel(unit, noLuaCall);
}

void CUnitDrawer::DrawUnitModelBeingBuiltOpaque(const CUnit* unit, bool noLuaCall)
{
	const S3DModel* model = unit->model;
	const    CTeam*  team = teamHandler->Team(unit->team);
	const   SColor  color = team->color;

	const float wireColorMult = std::fabs(128.0f - ((gs->frameNum * 4) & 255)) / 255.0f + 0.5f;
	const float flatColorMult = 1.5f - wireColorMult;

	const float3 frameColors[2] = {unit->unitDef->nanoColor, {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f}};
	const float3 stageColors[2] = {frameColors[globalRendering->teamNanospray], frameColors[globalRendering->teamNanospray]};
	const float3 stageBounds    = {0.0f, model->CalcDrawHeight(), unit->buildProgress};

	// draw-height defaults to maxs.y - mins.y, but can be overridden for non-3DO models
	// the default value derives from the model vertices and makes more sense to use here
	//
	// Both clip planes move up. Clip plane 0 is the upper bound of the model,
	// clip plane 1 is the lower bound. In other words, clip plane 0 makes the
	// wireframe/flat color/texture appear, and clip plane 1 then erases the
	// wireframe/flat color later on.
	const float4 upperPlanes[] = {
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f       )},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 1.0f)},
		{0.0f, -1.0f, 0.0f,  stageBounds.x + stageBounds.y * (stageBounds.z * 3.0f - 2.0f)},
		{0.0f,  0.0f, 0.0f,                                                          0.0f },
	};
	const float4 lowerPlanes[] = {
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z * 10.0f - 9.0f)},
		{0.0f,  1.0f, 0.0f, -stageBounds.x - stageBounds.y * (stageBounds.z *  3.0f - 2.0f)},
		{0.0f,  1.0f, 0.0f,                                  (                        0.0f)},
		{0.0f,  0.0f, 0.0f,                                                           0.0f },
	};

	enum {
		STAGE_WIRE = 0,
		STAGE_FLAT = 1,
		STAGE_FILL = 2,
		STAGE_NONE = 3,
	};

	// note: draw-func for stage i is at index i+1 (noop-func is at 0)
	DrawModelBuildStageFunc stageFunc = nullptr;
	IUnitDrawerState* selState = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);

	glEnable(GL_CLIP_DISTANCE0);
	glEnable(GL_CLIP_DISTANCE1);

	// wireframe, unconditional
	selState->SetNanoColor(float4(stageColors[0] * wireColorMult, 1.0f));
	stageFunc = drawModelBuildStageFuncs[(STAGE_WIRE + 1) * true];
	stageFunc(unit, selState, upperPlanes[STAGE_WIRE], lowerPlanes[STAGE_WIRE], noLuaCall);

	// flat-colored, conditional
	selState->SetNanoColor(float4(stageColors[1] * flatColorMult, 1.0f));
	stageFunc = drawModelBuildStageFuncs[(STAGE_FLAT + 1) * (stageBounds.z > 0.333f)];
	stageFunc(unit, selState, upperPlanes[STAGE_FLAT], lowerPlanes[STAGE_FLAT], noLuaCall);

	glDisable(GL_CLIP_DISTANCE1);

	// fully-shaded, conditional
	selState->SetNanoColor(float4(1.0f, 1.0f, 1.0f, 0.0f));
	stageFunc = drawModelBuildStageFuncs[(STAGE_FILL + 1) * (stageBounds.z > 0.666f)];
	stageFunc(unit, selState, upperPlanes[STAGE_FILL], lowerPlanes[STAGE_FILL], noLuaCall);

	glDisable(GL_CLIP_DISTANCE0);

	selState->SetBuildClipPlanes(upperPlanes[STAGE_NONE], lowerPlanes[STAGE_NONE]);
}




void CUnitDrawer::DrawUnitModel(const CUnit* unit, bool noLuaCall) {
	if (!noLuaCall && unit->luaDraw && eventHandler.DrawUnit(unit))
		return;

	unit->localModel.Draw();
}



void CUnitDrawer::SetUnitLuaTrans(const CUnit* unit, bool lodCall) {
	IUnitDrawerState* state = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);
	LocalModel* model = const_cast<LocalModel*>(&unit->localModel);

	// use whatever model transform is on the stack
	model->UpdatePieceMatrices(gs->frameNum);
	state->SetMatrices(GL::GetMatrix(GL_MODELVIEW), model->GetPieceMatrices());
}

void CUnitDrawer::SetUnitDefTrans(const CUnit* unit, bool lodCall) {
	IUnitDrawerState* state = unitDrawer->GetDrawerState(DRAWER_STATE_SEL);
	LocalModel* model = const_cast<LocalModel*>(&unit->localModel);

	model->UpdatePieceMatrices(gs->frameNum);
	state->SetMatrices(unit->GetTransformMatrix(), model->GetPieceMatrices());
}



void CUnitDrawer::DrawUnitLuaTrans(const CUnit* unit, bool lodCall, bool noLuaCall, bool fullModel) {
	// if called from LuaObjectDrawer, unit has a custom material
	//
	// we want Lua-material shaders to have full control over build
	// visualisation, so keep it simple and make LOD-calls draw the
	// full model
	//
	// NOTE: "raw" calls will no longer skip DrawUnitBeingBuilt
	//
	const unsigned int noNanoDraw = fullModel || !unit->beingBuilt || !unit->unitDef->showNanoFrame;
	const unsigned int shadowPass = shadowHandler->InShadowPass();

	const DrawModelFunc drawModelFunc = unitDrawer->GetDrawModelFunc(std::max(noNanoDraw * 2, shadowPass));

	SetUnitLuaTrans(unit, lodCall);
	drawModelFunc(unit, noLuaCall);
}

void CUnitDrawer::DrawUnitDefTrans(const CUnit* unit, bool lodCall, bool noLuaCall, bool fullModel) {
	const unsigned int noNanoDraw = lodCall || !unit->beingBuilt || !unit->unitDef->showNanoFrame;
	const unsigned int shadowPass = shadowHandler->InShadowPass();

	const DrawModelFunc drawModelFunc = unitDrawer->GetDrawModelFunc(std::max(noNanoDraw * 2, shadowPass));

	SetUnitDefTrans(unit, lodCall);
	drawModelFunc(unit, noLuaCall);
}




inline void CUnitDrawer::UpdateUnitIconState(CUnit* unit) {
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	// reset
	unit->isIcon = losStatus & LOS_INRADAR;

	if ((losStatus & LOS_INLOS) || gu->spectatingFullView)
		unit->isIcon = DrawAsIcon(unit, (unit->pos - camera->GetPos()).SqLength());

	if (!unit->isIcon)
		return;
	if (unit->noDraw)
		return;
	if (unit->IsInVoid())
		return;
	// drawing icons is cheap but not free, avoid a perf-hit when many are offscreen
	if (!camera->InView(unit->drawMidPos, unit->GetDrawRadius()))
		return;

	iconUnits.push_back(unit);
}

inline void CUnitDrawer::UpdateUnitDrawPos(CUnit* u) {
	const CUnit* t = u->GetTransporter();

	if (t != nullptr) {
		u->drawPos = u->GetDrawPos(t->speed, globalRendering->timeOffset);
	} else {
		u->drawPos = u->GetDrawPos(          globalRendering->timeOffset);
	}

	u->drawMidPos = u->GetMdlDrawMidPos();
}



bool CUnitDrawer::DrawAsIcon(const CUnit* unit, const float sqUnitCamDist) const {

	const float sqIconDistMult = unit->unitDef->iconType->GetDistanceSqr();
	const float realIconLength = iconLength * sqIconDistMult;

	bool asIcon = false;

	if (useDistToGroundForIcons) {
		asIcon = (sqCamDistToGroundForIcons > realIconLength);
	} else {
		asIcon = (sqUnitCamDist > realIconLength);
	}

	return asIcon;
}




void CUnitDrawer::SetupShowUnitBuildSquares(bool onMiniMap, bool testCanBuild)
{
	if (!testCanBuild)
		return;

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);


	GL::RenderDataBufferC* buffer = GL::GetRenderBufferC();
	Shader::IProgramObject* shader = buffer->GetShader();

	shader->Enable();
	shader->SetUniformMatrix4x4<const char*, float>("u_movi_mat", false, camera->GetViewMatrix());
	shader->SetUniformMatrix4x4<const char*, float>("u_proj_mat", false, camera->GetProjectionMatrix());
}

void CUnitDrawer::ResetShowUnitBuildSquares(bool onMiniMap, bool testCanBuild)
{
	GL::RenderDataBufferC* buffer = GL::GetRenderBufferC();
	Shader::IProgramObject* shader = buffer->GetShader();

	if (testCanBuild) {
		buffer->Submit(GL_QUADS);
		return;
	}

	buffer->Submit(GL_LINES);
	shader->Disable();


	glEnable(GL_DEPTH_TEST);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	// glDisable(GL_BLEND);
}


// visualize if a unit can be built at specified position by
// testing every square within a unit's build-grid footprint
// TODO: make this a lua callin!
bool CUnitDrawer::ShowUnitBuildSquares(const BuildInfo& buildInfo, const std::vector<Command>& commands, bool testCanBuild)
{
	GL::RenderDataBufferC* buffer = GL::GetRenderBufferC();

	const int x1 = buildInfo.pos.x - (buildInfo.GetXSize() * 0.5f * SQUARE_SIZE);
	const int z1 = buildInfo.pos.z - (buildInfo.GetZSize() * 0.5f * SQUARE_SIZE);
	const int x2 =              x1 + (buildInfo.GetXSize() *        SQUARE_SIZE);
	const int z2 =              z1 + (buildInfo.GetZSize() *        SQUARE_SIZE);
	const float h = CGameHelper::GetBuildHeight(buildInfo.pos, buildInfo.def, false);

	if (testCanBuild) {
		buildableSquares.clear();
		buildableSquares.reserve(32);
		featureSquares.clear(); // occupied by features
		featureSquares.reserve(32);
		illegalSquares.clear(); // otherwise non-buildable
		illegalSquares.reserve(32);

		CFeature* feature = nullptr;

		const bool canBuild = !!CGameHelper::TestUnitBuildSquare(
			buildInfo,
			feature,
			-1,
			false,
			&buildableSquares,
			&featureSquares,
			&illegalSquares,
			&commands
		);

		const SColor buildableSquareColors[] = {{0.9f, 0.8f, 0.0f, 0.7f}, {0.0f, 0.9f, 0.0f, 0.7f}};
		const SColor   featureSquareColors[] = {{0.9f, 0.8f, 0.0f, 0.7f}};
		const SColor   illegalSquareColors[] = {{0.9f, 0.0f, 0.0f, 0.7f}};

		for (unsigned int i = 0; i < buildableSquares.size(); i++) {
			buffer->SafeAppend({buildableSquares[i]                                      , buildableSquareColors[canBuild]});
			buffer->SafeAppend({buildableSquares[i] + float3(SQUARE_SIZE, 0,           0), buildableSquareColors[canBuild]});
			buffer->SafeAppend({buildableSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE), buildableSquareColors[canBuild]});
			buffer->SafeAppend({buildableSquares[i] + float3(          0, 0, SQUARE_SIZE), buildableSquareColors[canBuild]});
		}

		for (unsigned int i = 0; i < featureSquares.size(); i++) {
			buffer->SafeAppend({featureSquares[i]                                      , featureSquareColors[0]});
			buffer->SafeAppend({featureSquares[i] + float3(SQUARE_SIZE, 0,           0), featureSquareColors[0]});
			buffer->SafeAppend({featureSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE), featureSquareColors[0]});
			buffer->SafeAppend({featureSquares[i] + float3(          0, 0, SQUARE_SIZE), featureSquareColors[0]});
		}

		for (unsigned int i = 0; i < illegalSquares.size(); i++) {
			buffer->SafeAppend({illegalSquares[i]                                      , illegalSquareColors[0]});
			buffer->SafeAppend({illegalSquares[i] + float3(SQUARE_SIZE, 0,           0), illegalSquareColors[0]});
			buffer->SafeAppend({illegalSquares[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE), illegalSquareColors[0]});
			buffer->SafeAppend({illegalSquares[i] + float3(          0, 0, SQUARE_SIZE), illegalSquareColors[0]});
		}

		return canBuild;
	}

	if (h >= 0.0f)
		return false;

	constexpr unsigned char sc[4] = { 0,   0, 255, 128 }; // start color
	constexpr unsigned char ec[4] = { 0, 128, 255, 255 }; // end color

	// vertical lines
	buffer->SafeAppend({float3(x1, h, z1), sc}); buffer->SafeAppend({float3(x1, 0.0f, z1), ec});
	buffer->SafeAppend({float3(x1, h, z2), sc}); buffer->SafeAppend({float3(x1, 0.0f, z2), ec});
	buffer->SafeAppend({float3(x2, h, z2), sc}); buffer->SafeAppend({float3(x2, 0.0f, z2), ec});
	buffer->SafeAppend({float3(x2, h, z1), sc}); buffer->SafeAppend({float3(x2, 0.0f, z1), ec});

	// horizontal line-loop
	buffer->SafeAppend({float3(x1, 0.0f, z1), ec}); buffer->SafeAppend({float3(x1, 0.0f, z2), ec});
	buffer->SafeAppend({float3(x1, 0.0f, z2), ec}); buffer->SafeAppend({float3(x2, 0.0f, z2), ec});
	buffer->SafeAppend({float3(x2, 0.0f, z2), ec}); buffer->SafeAppend({float3(x2, 0.0f, z1), ec});
	buffer->SafeAppend({float3(x2, 0.0f, z1), ec}); buffer->SafeAppend({float3(x1, 0.0f, z1), ec});
	return false;
}



inline const icon::CIconData* GetUnitIcon(const CUnit* unit) {
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	const unsigned short prevMask = (LOS_PREVLOS | LOS_CONTRADAR);

	const UnitDef* unitDef = unit->unitDef;
	const icon::CIconData* iconData = nullptr;

	// use the unit's custom icon if we can currently see it,
	// or have seen it before and did not lose contact since
	const bool unitVisible = ((losStatus & (LOS_INLOS | LOS_INRADAR)) && ((losStatus & prevMask) == prevMask));
	const bool customIcon = (minimap->UseUnitIcons() && (unitVisible || gu->spectatingFullView));

	if (customIcon)
		return (unitDef->iconType.GetIconData());

	if ((losStatus & LOS_INRADAR) != 0)
		iconData = icon::iconHandler->GetDefaultIconData();

	return iconData;
}

inline float GetUnitIconScale(const CUnit* unit) {
	float scale = unit->myIcon->GetSize();

	if (!minimap->UseUnitIcons())
		return scale;
	if (!unit->myIcon->GetRadiusAdjust())
		return scale;

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];
	const unsigned short prevMask = (LOS_PREVLOS | LOS_CONTRADAR);
	const bool unitVisible = ((losStatus & LOS_INLOS) || ((losStatus & LOS_INRADAR) && ((losStatus & prevMask) == prevMask)));

	if (unitVisible || gu->spectatingFullView)
		scale *= (unit->radius / unit->myIcon->GetRadiusScale());

	return scale;
}


void CUnitDrawer::DrawUnitMiniMapIcon(const CUnit* unit, CVertexArray* va) const {
	if (unit->noMinimap)
		return;
	if (unit->myIcon == nullptr)
		return;
	if (unit->IsInVoid())
		return;

	const unsigned char defaultColor[4] = {255, 255, 255, 255};
	const unsigned char* color = &defaultColor[0];

	if (!unit->isSelected) {
		if (minimap->UseSimpleColors()) {
			if (unit->team == gu->myTeam) {
				color = minimap->GetMyTeamIconColor();
			} else if (teamHandler->Ally(gu->myAllyTeam, unit->allyteam)) {
				color = minimap->GetAllyTeamIconColor();
			} else {
				color = minimap->GetEnemyTeamIconColor();
			}
		} else {
			color = teamHandler->Team(unit->team)->color;
		}
	}

	const float iconScale = GetUnitIconScale(unit);
	const float3& iconPos = (!gu->spectatingFullView) ?
		unit->GetObjDrawErrorPos(gu->myAllyTeam):
		unit->GetObjDrawMidPos();

	const float iconSizeX = (iconScale * minimap->GetUnitSizeX());
	const float iconSizeY = (iconScale * minimap->GetUnitSizeY());

	const float x0 = iconPos.x - iconSizeX;
	const float x1 = iconPos.x + iconSizeX;
	const float y0 = iconPos.z - iconSizeY;
	const float y1 = iconPos.z + iconSizeY;

	unit->myIcon->DrawArray(va, x0, y0, x1, y1, color);
}

// TODO:
//   UnitDrawer::DrawIcon was half-duplicate of MiniMap::DrawUnit&co
//   the latter has been replaced by this, do the same for the former
//   (mini-map icons and real-map radar icons are the same anyway)
void CUnitDrawer::DrawUnitMiniMapIcons() const {
	CVertexArray* va = GetVertexArray();

	for (auto iconIt = unitsByIcon.cbegin(); iconIt != unitsByIcon.cend(); ++iconIt) {
		const icon::CIconData* icon = iconIt->first;
		const std::vector<const CUnit*>& units = iconIt->second;

		if (icon == nullptr)
			continue;
		if (units.empty())
			continue;

		va->Initialize();
		va->EnlargeArrays(units.size() * 4, 0, VA_SIZE_2DTC);
		icon->BindTexture();

		for (const CUnit* unit: units) {
			assert(unit->myIcon == icon);
			DrawUnitMiniMapIcon(unit, va);
		}

		va->DrawArray2dTC(GL_QUADS);
	}
}

void CUnitDrawer::UpdateUnitMiniMapIcon(const CUnit* unit, bool forced, bool killed) {
	CUnit* u = const_cast<CUnit*>(unit);

	icon::CIconData* oldIcon = unit->myIcon;
	icon::CIconData* newIcon = const_cast<icon::CIconData*>(GetUnitIcon(unit));

	u->myIcon = nullptr;

	if (!killed) {
		if ((oldIcon != newIcon) || forced) {
			spring::VectorErase(unitsByIcon[oldIcon], unit);
			unitsByIcon[newIcon].push_back(unit);
		}

		u->myIcon = newIcon;
		return;
	}

	spring::VectorErase(unitsByIcon[oldIcon], unit);
}



void CUnitDrawer::RenderUnitCreated(const CUnit* u, int cloaked) {
	CUnit* unit = const_cast<CUnit*>(u);

	if (u->model != nullptr) {
		if (cloaked) {
			alphaModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		} else {
			opaqueModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		}
	}

	UpdateUnitMiniMapIcon(u, false, false);
	unsortedUnits.push_back(unit);
}

void CUnitDrawer::RenderUnitDestroyed(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	const UnitDef* unitDef = unit->unitDef;
	const UnitDef* decoyDef = unitDef->decoyDef;

	const bool addNewGhost = unitDef->IsBuildingUnit() && gameSetup->ghostedBuildings;

	// TODO - make ghosted buildings per allyTeam - so they are correctly dealt with
	// when spectating
	GhostSolidObject* gso = nullptr;
	// FIXME -- adjust decals for decoys? gets weird?
	S3DModel* gsoModel = (decoyDef == nullptr)? u->model: decoyDef->LoadModel();

	for (int allyTeam = 0; allyTeam < deadGhostBuildings.size(); ++allyTeam) {
		const bool canSeeGhost = !(u->losStatus[allyTeam] & (LOS_INLOS | LOS_CONTRADAR)) && (u->losStatus[allyTeam] & (LOS_PREVLOS));

		if (addNewGhost && canSeeGhost) {
			if (gso == nullptr) {
				gso = new GhostSolidObject();
				gso->pos    = u->pos;
				gso->model  = gsoModel;
				gso->decal  = nullptr;
				gso->facing = u->buildFacing;
				gso->dir    = u->frontdir;
				gso->team   = u->team;
				gso->refCount = 0;
				gso->lastDrawFrame = 0;

				groundDecals->GhostCreated(u, gso);
			}

			// <gso> can be inserted for multiple allyteams
			// (the ref-counter saves us come deletion time)
			deadGhostBuildings[allyTeam][gsoModel->type].push_back(gso);
			gso->IncRef();
		}

		spring::VectorErase(liveGhostBuildings[allyTeam][MDL_TYPE(u)], u);
	}

	if (u->model != nullptr) {
		// delete from both; cloaked state is unreliable at this point
		alphaModelRenderers[MDL_TYPE(u)]->DelUnit(u);
		opaqueModelRenderers[MDL_TYPE(u)]->DelUnit(u);
	}

	spring::VectorErase(unsortedUnits, u);

	UpdateUnitMiniMapIcon(unit, false, true);
	LuaObjectDrawer::SetObjectLOD(u, LUAOBJ_UNIT, 0);
}


void CUnitDrawer::UnitCloaked(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	if (u->model != nullptr) {
		alphaModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		opaqueModelRenderers[MDL_TYPE(u)]->DelUnit(u);
	}
}

void CUnitDrawer::UnitDecloaked(const CUnit* unit) {
	CUnit* u = const_cast<CUnit*>(unit);

	if (u->model != nullptr) {
		opaqueModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		alphaModelRenderers[MDL_TYPE(u)]->DelUnit(u);
	}
}

void CUnitDrawer::UnitEnteredLos(const CUnit* unit, int allyTeam) {
	CUnit* u = const_cast<CUnit*>(unit); //cleanup

	if (gameSetup->ghostedBuildings && unit->unitDef->IsImmobileUnit())
		spring::VectorErase(liveGhostBuildings[allyTeam][MDL_TYPE(unit)], u);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitLeftLos(const CUnit* unit, int allyTeam) {
	CUnit* u = const_cast<CUnit*>(unit); //cleanup

	if (gameSetup->ghostedBuildings && unit->unitDef->IsImmobileUnit())
		spring::VectorInsertUnique(liveGhostBuildings[allyTeam][MDL_TYPE(unit)], u, true);

	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitEnteredRadar(const CUnit* unit, int allyTeam) {
	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}

void CUnitDrawer::UnitLeftRadar(const CUnit* unit, int allyTeam) {
	if (allyTeam != gu->myAllyTeam)
		return;

	UpdateUnitMiniMapIcon(unit, false, false);
}


void CUnitDrawer::PlayerChanged(int playerNum) {
	if (playerNum != gu->myPlayerNum)
		return;

	for (auto iconIt = unitsByIcon.begin(); iconIt != unitsByIcon.end(); ++iconIt) {
		(iconIt->second).clear();
	}

	for (CUnit* unit: unsortedUnits) {
		// force an erase (no-op) followed by an insert
		UpdateUnitMiniMapIcon(unit, true, false);
	}
}

void CUnitDrawer::SunChanged() {
	unitDrawerStates[DRAWER_STATE_SEL]->SetSkyLight(sky->GetLight());
}



bool CUnitDrawer::ObjectVisibleReflection(const float3 objPos, const float3 camPos, float maxRadius)
{
	if (objPos.y < 0.0f)
		return (CGround::GetApproximateHeight(objPos.x, objPos.z, false) <= maxRadius);

	const float dif = objPos.y - camPos.y;

	float3 zeroPos;
	zeroPos += (camPos * ( objPos.y / dif));
	zeroPos += (objPos * (-camPos.y / dif));

	return (CGround::GetApproximateHeight(zeroPos.x, zeroPos.z, false) <= maxRadius);
}



void CUnitDrawer::AddTempDrawUnit(const TempDrawUnit& tdu)
{
	const UnitDef* unitDef = tdu.unitDef;
	const S3DModel* model = unitDef->LoadModel();

	if (tdu.drawAlpha) {
		tempAlphaUnits[model->type].push_back(tdu);
	} else {
		tempOpaqueUnits[model->type].push_back(tdu);
	}
}

void CUnitDrawer::UpdateTempDrawUnits(std::vector<TempDrawUnit>& tempDrawUnits)
{
	for (unsigned int n = 0; n < tempDrawUnits.size(); /*no-op*/) {
		if (tempDrawUnits[n].timeout <= gs->frameNum) {
			// do not use spring::VectorErase; we already know the index
			tempDrawUnits[n] = tempDrawUnits.back();
			tempDrawUnits.pop_back();
			continue;
		}

		n += 1;
	}
}

