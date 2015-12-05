/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <string.h>

#include "Camera.h"
#include "UI/MouseHandler.h"
#include "Map/ReadMap.h"
#include "System/myMath.h"
#include "System/float3.h"
#include "System/Matrix44f.h"
#include "Rendering/GlobalRendering.h"
#include "System/Config/ConfigHandler.h"


CONFIG(float, EdgeMoveWidth)
	.defaultValue(0.02f)
	.minimumValue(0.0f)
	.description("The width (in percent of screen size) of the EdgeMove scrolling area.");
CONFIG(bool, EdgeMoveDynamic)
	.defaultValue(true)
	.description("If EdgeMove scrolling speed should fade with edge distance.");


CCamera* CCamera::camTypes[CCamera::CAMTYPE_COUNT] = {nullptr};


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CCamera::CCamera(unsigned int cameraType)
	: pos(ZeroVector)
	, rot(ZeroVector)
	, forward(RgtVector)
	, up(UpVector)
	, fov(0.0f)
	, halfFov(0.0f)
	, tanHalfFov(0.0f)
	, lppScale(0.0f)
	, posOffset(ZeroVector)
	, tiltOffset(ZeroVector)
	, camType(cameraType)
{
	// place us at center of the map
	pos = float3(mapDims.mapx * 0.5f * SQUARE_SIZE, 1000.f, mapDims.mapy * 0.5f * SQUARE_SIZE);

	memset(viewport, 0, 4 * sizeof(int));
	memset(movState, 0, sizeof(movState));
	memset(rotState, 0, sizeof(rotState));

	// stuff that will not change can be initialised here,
	// so it does not need to be reinitialised every update
	projectionMatrix[15] = 0.0f;
	billboardMatrix[15] = 1.0f;

	SetFov(45.0f);
}

void CCamera::CopyState(const CCamera* cam) {
	for (unsigned int i = 0; i < FRUSTUM_PLANE_CNT; i++) {
		frustumPlanes[i] = cam->frustumPlanes[i];
	}

	forward   = cam->GetForward();
	right     = cam->GetRight();
	up        = cam->GetUp();

	pos       = cam->GetPos();
	rot       = cam->GetRot();

	lppScale  = cam->GetLPPScale();
	camType   = cam->GetCamType();
}

void CCamera::Update(bool updateDirs)
{
	if (updateDirs) {
		UpdateDirsFromRot(rot);
	}

	if (globalRendering->viewSizeY <= 0) {
		lppScale = 0.0f;
	} else {
		lppScale = (2.0f * tanHalfFov) / globalRendering->viewSizeY;
	}

	ComputeViewRange();
	UpdateFrustum();
	UpdateMatrices();

	// viewport
	viewport[0] = 0;
	viewport[1] = 0;
	viewport[2] = globalRendering->viewSizeX;
	viewport[3] = globalRendering->viewSizeY;
}

void CCamera::UpdateFrustum()
{
	// NOTE: "-" because we want normals
	const float3 forwardy = (-forward *                                          tanHalfFov );
	const float3 forwardx = (-forward * math::tan(globalRendering->aspectRatio *    halfFov));

	frustumPlanes[FRUSTUM_PLANE_TOP] = (forwardy +    up).UnsafeANormalize();
	frustumPlanes[FRUSTUM_PLANE_BOT] = (forwardy -    up).UnsafeANormalize();
	frustumPlanes[FRUSTUM_PLANE_RGT] = (forwardx + right).UnsafeANormalize();
	frustumPlanes[FRUSTUM_PLANE_LFT] = (forwardx - right).UnsafeANormalize();

	if (camType == CAMTYPE_PLAYER || camType == CAMTYPE_SHADOW) {
		// vis-culling is always performed from player's (or light's)
		// POV; copy over the frustum planes we just calculated above
		// note that this is the only place where VISCUL is updated!
		camTypes[CAMTYPE_VISCUL]->CopyState(camTypes[camType]);
	}
}

void CCamera::UpdateMatrices()
{
	// store and apply the projection transform
	myGluPerspective(globalRendering->aspectRatio, globalRendering->zNear, globalRendering->viewRange);

	// FIXME:
	//   should be applying the offsets to pos/up/right/forward/etc,
	//   but without affecting the real positions (need an intermediary)
	const float3 fShake = ((forward * (1.0f + tiltOffset.z)) + (right * tiltOffset.x) + (up * tiltOffset.y)).ANormalize();
	const float3 camPos = pos + posOffset;
	const float3 center = camPos + fShake;

	// store and apply the view transform
	myGluLookAt(camPos, center, up);


	// create extra matrices (useful for shaders)
	viewProjectionMatrix = projectionMatrix * viewMatrix;
	viewMatrixInverse = viewMatrix.InvertAffine();
	projectionMatrixInverse = projectionMatrix.Invert();
	viewProjectionMatrixInverse = viewProjectionMatrix.Invert();

	// Billboard Matrix
	billboardMatrix = viewMatrix;
	billboardMatrix.SetPos(ZeroVector);
	billboardMatrix.Transpose(); // viewMatrix is affine, equals inverse
	billboardMatrix[15] = 1.0f; // SetPos() touches m[15]
}


void CCamera::ComputeViewRange()
{
	float wantedViewRange = CGlobalRendering::MAX_VIEW_RANGE;

	const float azimuthCos       = forward.dot(UpVector);
	const float maxDistToBorderX = std::max(pos.x, float3::maxxpos - pos.x);
	const float maxDistToBorderZ = std::max(pos.z, float3::maxzpos - pos.z);
	const float minViewRange     = (1.0f - azimuthCos) * math::sqrt(Square(maxDistToBorderX) + Square(maxDistToBorderZ));

	// Camera-height dependent (i.e. TAB-view)
	wantedViewRange = std::max(wantedViewRange, (pos.y - std::max(0.0f, readMap->GetCurrMinHeight())) * 2.4f);
	// View-angle dependent (i.e. FPS-view)
	wantedViewRange = std::max(wantedViewRange, minViewRange);

	// Update
	const float factor = wantedViewRange / CGlobalRendering::MAX_VIEW_RANGE;

	globalRendering->zNear     = CGlobalRendering::NEAR_PLANE * factor;
	globalRendering->viewRange = CGlobalRendering::MAX_VIEW_RANGE * factor;
}


static inline bool AABBInOriginPlane(
	const float3& plane,
	const float3& camPos,
	const float3& mins,
	const float3& maxs
) {
	float3 fp; // far point
	fp.x = (plane.x > 0.0f) ? mins.x : maxs.x;
	fp.y = (plane.y > 0.0f) ? mins.y : maxs.y;
	fp.z = (plane.z > 0.0f) ? mins.z : maxs.z;
	return (plane.dot(fp - camPos) < 0.0f);
}


bool CCamera::InView(const float3& mins, const float3& maxs) const
{
	// axis-aligned bounding box test (AABB)
	for (unsigned int i = 0; i < FRUSTUM_PLANE_CNT; i++) {
		if (!AABBInOriginPlane(frustumPlanes[i], pos, mins, maxs)) {
			return false;
		}
	}

	return true;
}

bool CCamera::InView(const float3& p, float radius) const
{
	const float3 vec = p - pos;

	for (unsigned int i = 0; i < FRUSTUM_PLANE_CNT; i++) {
		if (vec.dot(frustumPlanes[i]) > radius) {
			return false;
		}
	}

	// final test against base-plane
	return (vec.SqLength() <= Square(globalRendering->viewRange + radius));
}


void CCamera::SetFov(const float myfov)
{
	fov = myfov;
	halfFov = (fov * 0.5f) * (PI / 180.f);
	tanHalfFov = math::tan(halfFov);
}



float3 CCamera::GetRotFromDir(float3 fwd)
{
	fwd.Normalize();

	// NOTE:
	//   atan2(0.0,  0.0) returns 0.0
	//   atan2(0.0, -0.0) returns PI
	//   azimuth (yaw) 0 is on negative z-axis
	//
	float3 r;
	r.x = math::acos(fwd.y);
	r.y = math::atan2(fwd.x, -fwd.z);
	r.z = 0.0f;
	return r;
}

float3 CCamera::GetFwdFromRot(const float3 r)
{
	float3 fwd;
	fwd.x = math::sin(r.x) *   math::sin(r.y);
	fwd.z = math::sin(r.x) * (-math::cos(r.y));
	fwd.y = math::cos(r.x);
	return fwd;
}

float3 CCamera::GetRgtFromRot(const float3 r)
{
	// FIXME:
	//   right should always be "right" relative to forward
	//   (i.e. up should always point "up" in WS and camera
	//   can not flip upside down) but is not
	//
	//   fwd=(0,+1,0) -> rot=GetRotFromDir(fwd)=(0.0, PI, 0.0) -> GetRgtFromRot(rot)=(-1.0, 0.0, 0.0)
	//   fwd=(0,-1,0) -> rot=GetRotFromDir(fwd)=( PI, PI, 0.0) -> GetRgtFromRot(rot)=(+1.0, 0.0, 0.0)
	//
	float3 rgt;
	rgt.x = math::sin(HALFPI - r.z) *   math::sin(r.y + HALFPI);
	rgt.z = math::sin(HALFPI - r.z) * (-math::cos(r.y + HALFPI));
	rgt.y = math::cos(HALFPI - r.z);
	return rgt;
}


void CCamera::UpdateDirsFromRot(const float3 r)
{
	forward  = std::move(GetFwdFromRot(r));
	right    = std::move(GetRgtFromRot(r));
	up       = (right.cross(forward)).Normalize();
}

void CCamera::SetDir(const float3 dir)
{
	// if (dir == forward) return;
	// update our axis-system from the angles
	SetRot(GetRotFromDir(dir) + (FwdVector * rot.z));
	assert(dir.dot(forward) > 0.9f);
}



float3 CCamera::CalcPixelDir(int x, int y) const
{
	const int vsx = std::max(1, globalRendering->viewSizeX);
	const int vsy = std::max(1, globalRendering->viewSizeY);

	const float dx = float(x - globalRendering->viewPosX - (vsx >> 1)) / vsy * (tanHalfFov * 2.0f);
	const float dy = float(y -                             (vsy >> 1)) / vsy * (tanHalfFov * 2.0f);

	const float3 dir = (forward - up * dy + right * dx).Normalize();
	return dir;
}


float3 CCamera::CalcWindowCoordinates(const float3& objPos) const
{
	// does same as gluProject()
	const float4 v = viewProjectionMatrix * float4(objPos, 1.0f);
	float3 winPos;
	winPos.x = viewport[0] + viewport[2] * (v.x / v.w + 1.0f) * 0.5f;
	winPos.y = viewport[1] + viewport[3] * (v.y / v.w + 1.0f) * 0.5f;
	winPos.z =                             (v.z / v.w + 1.0f) * 0.5f;
	return winPos;
}


inline void CCamera::myGluPerspective(float aspect, float zNear, float zFar) {
	const float t = zNear * tanHalfFov;
	const float b = -t;
	const float l = b * aspect;
	const float r = t * aspect;

	projectionMatrix[ 0] = (2.0f * zNear) / (r - l);

	projectionMatrix[ 5] = (2.0f * zNear) / (t - b);

	projectionMatrix[ 8] = (r + l) / (r - l);
	projectionMatrix[ 9] = (t + b) / (t - b);
	projectionMatrix[10] = -(zFar + zNear) / (zFar - zNear);
	projectionMatrix[11] = -1.0f;

	projectionMatrix[14] = -(2.0f * zFar * zNear) / (zFar - zNear);

	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(projectionMatrix);
}


inline void CCamera::myGluLookAt(const float3& eye, const float3& center, const float3& up) {
	const float3 f = (center - eye).ANormalize();
	const float3 s = f.cross(up);
	const float3 u = s.cross(f);

	viewMatrix[ 0] =  s.x;
	viewMatrix[ 1] =  u.x;
	viewMatrix[ 2] = -f.x;

	viewMatrix[ 4] =  s.y;
	viewMatrix[ 5] =  u.y;
	viewMatrix[ 6] = -f.y;

	viewMatrix[ 8] =  s.z;
	viewMatrix[ 9] =  u.z;
	viewMatrix[10] = -f.z;

	// save a glTranslated(-eye.x, -eye.y, -eye.z) call
	viewMatrix[12] = ( s.x * -eye.x) + ( s.y * -eye.y) + ( s.z * -eye.z);
	viewMatrix[13] = ( u.x * -eye.x) + ( u.y * -eye.y) + ( u.z * -eye.z);
	viewMatrix[14] = (-f.x * -eye.x) + (-f.y * -eye.y) + (-f.z * -eye.z);

	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(viewMatrix);
}



void CCamera::GetFrustumSides(float miny, float maxy, float scale, bool negSide) {

	ClearFrustumSides();

	// note: order does not matter
	for (unsigned int i = 0; i < FRUSTUM_PLANE_CNT; i++) {
		GetFrustumSide(frustumPlanes[i], ZeroVector,  miny, maxy, scale,  (frustumPlanes[i].y > 0.0f), negSide);
	}
}

void CCamera::GetFrustumSide(
	const float3& zdir,
	const float3& offset,
	float miny,
	float maxy,
	float scale,
	bool upwardDir,
	bool negSide)
{
	// compose an orthonormal axis-system around <zdir>
	float3 xdir = (zdir.cross(UpVector)).UnsafeANormalize();
	float3 ydir = (zdir.cross(xdir)).UnsafeANormalize();

	// intersection of vector from <pos> along <ydir> with xz-plane
	float3 pInt;

	// prevent DIV0 when calculating line.dir
	if (math::fabs(xdir.z) < 0.001f)
		xdir.z = 0.001f;

	if (ydir.y != 0.0f) {
		// if <zdir> is angled toward the sky instead of the ground,
		// subtract <miny> from the camera's y-position, else <maxy>
		if (upwardDir) {
			pInt = (pos + offset) - ydir * ((pos.y - miny) / ydir.y);
		} else {
			pInt = (pos + offset) - ydir * ((pos.y - maxy) / ydir.y);
		}
	}

	// <line.dir> is the direction coefficient (0 ==> parallel to z-axis, inf ==> parallel to x-axis)
	// in the xz-plane; <line.base> is the x-coordinate at which line intersects x-axis; <line.sign>
	// indicates line direction, ie. left-to-right (whenever <xdir.z> is negative) or right-to-left
	// NOTE:
	//     (b.x / b.z) is actually the reciprocal of the DC (ie. the number of steps along +x for
	//     one step along +y); the world z-axis is inverted wrt. a regular Carthesian grid, so the
	//     DC is also inverted
	FrustumLine line;
	line.dir  = (xdir.x / xdir.z);
	line.base = (pInt.x - (pInt.z * line.dir)) / scale;
	line.sign = (xdir.z <= 0.0f)? 1: -1;
	line.minz = (                      0.0f) - (mapDims.mapy);
	line.maxz = (mapDims.mapy * SQUARE_SIZE) + (mapDims.mapy);

	if (line.sign == 1 || negSide) {
		negFrustumSides.push_back(line);
	} else {
		posFrustumSides.push_back(line);
	}
}

void CCamera::ClipFrustumLines(bool neg, const float zmin, const float zmax) {

	std::vector<FrustumLine>& lines = neg? negFrustumSides: posFrustumSides;
	std::vector<FrustumLine>::iterator fli, fli2;

	for (fli = lines.begin(); fli != lines.end(); ++fli) {
		for (fli2 = lines.begin(); fli2 != lines.end(); ++fli2) {
			if (fli == fli2)
				continue;

			const float dbase = fli->base - fli2->base;
			const float ddir = fli->dir - fli2->dir;

			if (ddir == 0.0f)
				continue;

			const float colz = -(dbase / ddir);

			if ((fli2->sign * ddir) > 0.0f) {
				if ((colz > fli->minz) && (colz < zmax))
					fli->minz = colz;
			} else {
				if ((colz < fli->maxz) && (colz > zmin))
					fli->maxz = colz;
			}
		}
	}
}



float CCamera::GetMoveDistance(float* time, float* speed, int idx) const
{
	float camDeltaTime = globalRendering->lastFrameTime;
	float camMoveSpeed = 1.0f;

	camMoveSpeed *= (1.0f - movState[MOVE_STATE_SLW] * 0.9f);
	camMoveSpeed *= (1.0f + movState[MOVE_STATE_FST] * 9.0f);

	if (time != NULL) { *time = camDeltaTime; }
	if (speed != NULL) { *speed = camMoveSpeed; }

	switch (idx) {
		case MOVE_STATE_UP:  { camMoveSpeed *=  float(movState[idx]); } break;
		case MOVE_STATE_DWN: { camMoveSpeed *= -float(movState[idx]); } break;

		default: {
		} break;
	}

	return (camDeltaTime * 0.2f * camMoveSpeed);
}

float3 CCamera::GetMoveVectorFromState(bool fromKeyState) const
{
	float camDeltaTime = 1.0f;
	float camMoveSpeed = 1.0f;

	(void) GetMoveDistance(&camDeltaTime, &camMoveSpeed, -1);

	float3 v;
	if (fromKeyState) {
		v.y += (camDeltaTime * 0.001f * movState[MOVE_STATE_FWD]);
		v.y -= (camDeltaTime * 0.001f * movState[MOVE_STATE_BCK]);
		v.x += (camDeltaTime * 0.001f * movState[MOVE_STATE_RGT]);
		v.x -= (camDeltaTime * 0.001f * movState[MOVE_STATE_LFT]);
	} else {
		const int screenH = globalRendering->viewSizeY;
		const int screenW = globalRendering->dualScreenMode?
			(globalRendering->viewSizeX << 1):
			(globalRendering->viewSizeX     );

		const float width  = configHandler->GetFloat("EdgeMoveWidth");
		const bool dynamic = configHandler->GetBool("EdgeMoveDynamic");

		int2 border;
		border.x = std::max<int>(1, screenW * width);
		border.y = std::max<int>(1, screenH * width);

		float2 distToEdge; // must be float, ints don't save the sign in case of 0 and we need it for copysign()
		distToEdge.x = Clamp(mouse->lastx, 0, screenW);
		distToEdge.y = Clamp(mouse->lasty, 0, screenH);
		if (((screenW-1) - distToEdge.x) < distToEdge.x) distToEdge.x = -((screenW-1) - distToEdge.x);
		if (((screenH-1) - distToEdge.y) < distToEdge.y) distToEdge.y = -((screenH-1) - distToEdge.y);
		distToEdge.x = -distToEdge.x;

		float2 move;
		if (dynamic) {
			move.x = Clamp(float(border.x - std::abs(distToEdge.x)) / border.x, 0.f, 1.f);
			move.y = Clamp(float(border.y - std::abs(distToEdge.y)) / border.y, 0.f, 1.f);
		} else {
			move.x = int(std::abs(distToEdge.x) < border.x);
			move.y = int(std::abs(distToEdge.y) < border.y);
		}
		move.x = std::copysign(move.x, distToEdge.x);
		move.y = std::copysign(move.y, distToEdge.y);

		v.x = (camDeltaTime * 0.001f * move.x);
		v.y = (camDeltaTime * 0.001f * move.y);
	}

	v.z = camMoveSpeed;
	return v;
}

